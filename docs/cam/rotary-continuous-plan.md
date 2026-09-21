# Continuous 4-axis: a spiral that turns while it cuts

## The problem

`surface_rotary` is a rotary RASTER. Every pass runs along X at one A; the
part turns only on the link between passes. The posted rolling pin changes A
319 times, all 319 on rapids, none while cutting. That is a step past indexed
4-axis, and it is not simultaneous 4-axis. Its own `describe` row, the v122
format row and `dogfood-rotary.md` call it "continuous", which is wrong.

What that costs:

- **No helical feature can be cut as one.** Threads, augers, rope twists,
  spiral grooves, a barley twist's flutes. The pin's flutes are helical and
  every pass crosses them at an angle, which is where its scallops come from:
  320 passes, 76 minutes, 2.45 mm left on the worst cell.
- **Every pass is a seam.** A retract, an index and a plunge, 320 times.
- **Nothing that must move A and X together** (wrapped engraving, text round
  a cylinder) is reachable at all.

The checking side is ALREADY continuous. `CarveSweepRotary` interpolates A
along a move, the timeline gives a turning move its arc length, `RotaryStockSim`
plays it, `InspectRotary` checks its rapids and the parser reads a modal A. So
the work is a generator and the machine-facing questions it raises. Neither
engine nor picture changes.

## What the sources do (`~/Documents/CAD/CAM`)

Read for this plan: FreeCAD's `rotary_spiral.py`, `rotary_rings.py`,
`rotary_wrap.py` and `Op/RotarySurface.py`, and Kiri:Moto's `topo4.js`.

1. **A spiral is one revolution per axial stepover, reading the radius at
   every point.** FreeCAD walks x from `x_min` to `x_max` while θ runs through
   `(x_max − x_min) / stepover` turns, at a fixed angular step. Each point takes
   its radius from the pre-sampled cutter-location grid by bilinear lookup.
   Cells with no data get the grid's largest radius. **It never lifts
   mid-spiral**, and it doesn't need to: the cutter-location surface already
   rides over everything, which is the property our `RotaryCL` has.
   Its ends are the weak point. The helix starts exactly at `x_min`, so the
   first and last half-turns get only the tool's radius of coverage. Kiri's
   "rings" pattern (a closed revolution at each X) does not have that problem.
2. **Rotary roughing is the same generator with a floor.** FreeCAD and Kiri
   both cut layers at `r = max(cl, r_blank − k·stepdown)`, outermost first,
   and the last layer IS the finish. Neither has a separate roughing strategy.
3. **Neither feeds a mixed move correctly.** FreeCAD's default (`AxialOnly`)
   emits the plain F. Its `SurfaceSpeed` mode scales every move by
   `360 / 2πr`, which is right only for a pure rotation. Kiri emits the plain
   F. Nobody emits G93. The correct F depends on how the CONTROLLER measures a
   move that mixes millimetres and degrees, and the two families differ:
   Mach3 feeds along the combined vector, counting a degree as a millimetre;
   LinuxCNC feeds the XYZ part when there is one and the rotary only when it
   moves alone (RS274NGC 2.1.2.5).
4. **A is unwound.** Both emit a monotone A across revolutions, and FreeCAD
   rewrites it per machine afterwards: unwound, modulo 360, or "rezero" (a
   `G92 A` at each revolution boundary). For us the scale is the point. The
   pin at a 0.5 mm pitch is 600 turns, which leaves A at 216,000°. The next
   operation's rapid to A89 would unwind all of it.

## The shape of the fix

### S1. The spiral pattern

`RotarySpiralPath(cl, req)` in `CamSurface`, next to `RotaryParallelPasses`,
returns one polyline of `(x, r, θ)` in part angles.

- **Pitch** is the spacing between turns, from the op's `stepover` as a
  fraction of tool diameter, and it is measured ALONG THE SURFACE. On a flat
  stretch that is the axial advance per turn. Where the surface climbs, the
  advance shrinks by `1 / sqrt(1 + s²)`, with `s` the steepest slope within
  a pitch either side of the cutter. A fixed axial pitch crossed the test
  shaft's shoulders in 3 mm steps and left 3.7 mm there against the
  parallel passes' 1.1. Looking only ahead of the cutter still left 2.2,
  because the pitch sprang back to full at the foot of each wall. Looking
  both ways leaves 0.96, under the parallel passes.
- **Angular step** is derived from the grid, not set: the arc between two
  points at the part's widest radius is no longer than `sampleStep` (or the
  cell size when that is zero). This is the same rule `RotaryParallelRequest`
  already uses to turn an angle into a spacing.
- **Radius** comes from a bilinear lookup in the CL map, wrapping in θ.
  Bilinear can dip below the true surface inside a concave cell. The verifier
  measures that, and the gate below bounds it.
- **Empty cells** (the tool reaches nothing) ride at the blank radius. The
  spiral cannot break without a relink, and cutting air there is what a break
  would cost anyway.
- **Ends:** one closed revolution at constant x at each end, Kiri's ring,
  which fixes FreeCAD's half-turns.
- **Decimation:** drop a point when it lies on the line through its
  neighbours in `(x, θ, r)` within a tolerance of `cusp / 4`. The pin is mostly
  flutes, so this is for turned regions and should not be counted on for size.
- **Direction:** θ increases along the helix, so A = 90 − θ decreases,
  the same sense the parallel passes step in. There is no climb field on a
  rotary op, and this round adds none.

`GenerateRotarySurface` gains a `pattern` switch: `parallel` (today's raster)
or `spiral`. A new operation field `rotary_pattern` is refused when unknown,
naming both. It is a format change: v123 plus a golden.

### S2. Roughing layers

`surface_rotary` takes `OpField::Stepdown`. With a stepdown, layers run from
the blank radius inward (`MakeRotaryBlank`'s radius, the blank the verifier
already uses), each at `max(cl, r_blank − k·stepdown)`. The last layer is the
surface. This works for both patterns. It is the first rotary roughing we
have, and it costs one clamp in the lookup. Each spiral layer starts on the
turn the one before ended on, so the rapid between two layers turns the part
less than a revolution rather than unwinding the whole layer.

### S3. Feed compensation, in the post

A move whose A changes while cutting is posted with

    F = f · L_controller / L_true

where `L_true` is the path the tool tip really travels: the linear part
combined with the arc at radius `hypot(y, z)`, which is what `CuttingMinutes`
already measures. `L_controller` is what the post's dialect measures:

- `mach3`: `sqrt(dx² + dy² + dz² + dA²)`, with A in degrees.
- `linuxcnc`: `|dxyz|` when non-zero, else `|dA|`.

This lives in the POST, not the generator, because it is a fact about the
controller. Every future move that cuts while turning (wrapped engraving
included) gets it without asking.

F is then held to what the AXES can do, each to its own top speed, which is
how the controller limits a move: X, Y and Z to the machine's `rapid_feed`,
A to a new machine field, `a_max_deg_min` (the A motor's velocity in the
controller's tuning). The first version capped the whole F at `rapid_feed`.
On Mach3's combined vector a mostly-turning move's F is essentially A's speed
in degrees per minute, so that held A to 1500°/min, a limit the 4th axis
does not have. On paper the rolling pin's spiral became a 36-hour job. The
hold is reported ONCE per operation: how many moves, the smallest radius, and
the feed they would have needed. Where the machine states no A top speed,
nothing holds A, and the verdict says once what the program asks of it
("turns A at up to N deg/min"), which is the number to compare with the motor
tuning. The file is in `src/core/cam/CamRotaryFeed.{h,cpp}`, built without
the engine, because the post is part of the document model in every
configuration.

`ControllerFeed` (in `CamGcode`, beside the parser) serves the post,
`CuttingMinutes` and the verdict's cap warning. `BuildTimeline` times a
turning cut with `ControllerLength` over the file's F. So the cycle time
shown is the one the controller will take, including the moves that were
clamped. `cam_post` reports it as `cutting_minutes`.

The post needed one more thing: a pure turn of the part followed by a
straight feed was merged into one move doing both at once, because the run
merger looked only at XYZ. A move that changes A now never merges forward.
Nothing had ever emitted that pair before the spiral.

**Set `a_max_deg_min` on the machine** from the A motor's velocity in the
controller's tuning, or the cycle time assumes the axis keeps up.

**Confirm on the machine before trusting it:** `G1 X10 A360 F100` from rest.
Mach3's rule takes 3.6 minutes (a 360.1 "mm" vector). LinuxCNC's rule takes
6 seconds (10 mm of XYZ). One timed move settles which formula the 6090's
controller actually uses.

### S4. A winding

At the end of any operation that leaves A a turn or more from where it
started, the post writes `G92 A<v>`, where `v` is the current angle moved by
whole turns to lie nearest the next angle the file needs. So the next
operation's approach turns the part less than half a revolution. The offset
is a whole number of turns, so it names the same physical angle; it touches A
only; Mach3 and LinuxCNC both honour it. The parser reads `G92 A` as a rename,
keeping its moves on the continuous angle, so the verifier and the simulator
stay in step with the machine. `G92` on a linear axis is reported as not
modelled rather than read as a move.

There is no modulo-360 output. It looks like the answer for a controller
that rolls A over itself, and it is wrong for a cut: a feed move from 359° to
1° turns back 358°, tool down, on any controller that does not take the
short way.

### Not in this round

Paths that follow a feature (the flute itself, wrapped text) instead of a
uniform spiral. That is a different generator: it needs the part's geometry
in `(x, θ)`, not only its radius. It comes after this round, and this round's
post, feed and winding work is what it will sit on.

## Files

- `src/core/cam/CamSurface.{h,cpp}`: `RotarySpiralPath`, the lookup, the
  decimation.
- `src/core/cam/CamStrategies.cpp`: the pattern switch, the layers, the
  spiral's approach and retract in `GenerateRotarySurface`.
- `src/core/cam/CamJob.h`, `CamNames.cpp`: `RotaryPattern`, the field and
  its names, `Stepdown` on `surface_rotary`.
- `src/core/cam/CamRotary.{h,cpp}`: `RotaryBlankRadius`, the one rule for
  the blank's radius, shared by `MakeRotaryBlank` and the layers.
- `src/core/io/SerializeCam.cpp`, `SerializationInternal.h`: v123.
- `src/core/cam/CamPost.cpp`: the scaled F, the clamp comment, `G92 A`,
  the index-only retract, no merging across a turn.
- `src/core/cam/CamGcode.cpp`: `G92 A` in the parser.
- `src/core/cam/CamRotaryFeed.{h,cpp}` (new, built in every configuration):
  `FeedModelOf`, `TipLength`, `ControllerLength`, `AxisLimits`,
  `ControllerFeed`.
- `src/core/cam/CamJob.h`, `SerializeCam.cpp`, `SettingsWindow.cpp`:
  `Machine::rotaryMaxFeed`, saved as `a_max_deg_min` and edited beside the
  rapid feed.
- `src/core/cam/CamSim.cpp`, `CamVerify.cpp`: the timeline's clock; the
  once-per-op cap warning.
- `src/core/cam/CamDerive.cpp`: `CuttingMinutes` through `ControllerFeed`.
- `src/ui/CamPanel.cpp`, `CamPlayback.cpp`: the pattern radio pair, through
  the capability table; the playback clock's feed model.
- `src/remote/RemoteCam.cpp`: `rotary_pattern` on the op (its stepdown is the
  existing `stepdown`), `cutting_minutes` on `cam_post`, and a `describe` row
  that says what the op is.
- `generators/rotary/rolling_pin_job.py`: a `--spiral` switch.

## How it is verified

- **Spiral geometry:** x advances exactly one pitch per 360° of θ; both end
  rings are closed at constant x; empty cells ride at the blank radius;
  decimation stays inside its tolerance against the undecimated path.
- **One engine still:** a spiral program simulated and verified gives
  bit-identical stock, using the existing `sim == verify` harness, and
  scrubbing back matches playing forward.
- **Cut quality, by number:** on the handle and the pin, spiral finish
  leftover ≤ the parallel finish's, and gouge within the same bound the
  parallel gate holds.
- **Feed:** a pure-A move at radius r posts `f·360/2πr` on mach3; a pure-X
  move posts f; a mixed move posts the ratio above on each dialect; clamped
  moves are counted and reported once; the posted file clocked the
  controller's way equals `CuttingMinutes`.
- **Winding:** a job with a spiral followed by a parallel op, posted, parsed
  and verified, lands every move at the same physical angle, and the second
  op's first rapid turns less than one revolution.
- **Roughing:** layer k never cuts below `r_blank − k·stepdown`, and the last
  layer equals the unlayered finish.
- **GUI:** a radio pair on the op's Advanced row, not a combo, because a
  combo gathers with an empty label and the test engine cannot reach it.
  `cam/a_rotary_op_spirals_from_the_panel` clicks "spiral" and requires the
  regenerated path to turn while it cuts.
- **Format:** the v123 golden and the history row.
- **The dogfood:** the rolling pin regenerated with the spiral, against
  today's 76 min, 172,807 lines and 2.45 mm leftover, recorded in
  `dogfood-rotary.md`.
- **Docs:** `rotary-plan.md` (D2 closed, the "continuous" claim corrected),
  `format.md`, `dogfood-rotary.md`'s title, and the `describe` rows.
