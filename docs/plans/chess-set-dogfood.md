# Chess set dogfood: what is left

The set is designed and every program is generated
([generators/chess](../../generators/chess/README.md); `run_all.sh PORT`
rebuilds all of it from nothing into `scratch/chess/`). What remains is the
machine and the CAM gaps the round found. Text is last.

## 0. At the machine (the user)

1. **The rotary as installed.** Jog A +90 and watch which way a mark on the
   chuck's top moves; read the axis letter and steps/degree from Mach3's
   motor tuning; measure the chuck's diameter, its grip, the chuck-to-centre
   length, and the real rapid feed. They go into `MACHINE` in
   `generators/chess/cam.py`. Until then no rotary program is for the machine.
2. **The pawn blank in cheap wood**, rough and finish only (`-T1`, `-T2`);
   calipers on the first pawn against the drawing.

The pawn blank has no rest pass: its profile is finishable by the 3 mm ball.

## 4. CAM

- **CAM calls that do not stall the frame.** `cam_toolpath`, `cam_verify`,
  `cam_post` and `cam_simulate` run on the main thread (the watchdog logged
  60-330 s). The server already parks requests across frames for
  screenshots and UI actions; a CAM request parks the same way: queue the
  passes for every setup it needs (`CamToolpathsQueued`), keep the frame
  running, and answer when the `cam` jobs are done, with verification as the
  `cam-verify` background job the panel already uses. Callers keep
  synchronous semantics; `async: true` stays for pollers.
- **Stock carried everywhere.** The playback (`BeginCamPlayback`) starts
  from what earlier setups left, through the same `SeedFromEarlier` the
  planar check uses. An indexed operation after a turning setup derives
  `top_z` from the turned envelope over its target (the model's largest
  radius along the op's span), not the fresh bar, so the crenel, mitre and
  cross ops need no typed depths.
- **One operation at several rotary indices.** `Operation` gains an index
  list (or count and step); generation runs once per angle with that angle's
  frame and joins the pieces with a retract; the post already writes a new
  A per move and retracts before turning; the verify reads such a file
  through the rotary path. The queen's eight crown setups become one.
- **A spiral rest pass only where the rest is:** one spiral per leftover
  band, retracting between, instead of one winding the whole length at the
  rotary's speed limit; and `holding_clipped_mm` counts a spiral's clipped
  length correctly.

Tests: a parked `cam_verify` answers while `ping` answers in between;
playback of an indexed setup after a turning one starts from the turned
stock; a four-index crenel op posts four A words and verifies clean; the
knights blank's spiral rest cuts only its bands.

## 5. Text

The board's a-h and 1-8, engraved. Two layers, in this order:

- **A text sketch entity** (string, font, height, position, angle, anchor)
  that regenerates into curves the existing features already consume: extrude
  for raised letters, cut for recessed, construction for guides. In the
  sketch, not a feature of its own, so a label is dimensioned and constrained
  like anything else and its faces are named for CAM to pick. Outline glyphs
  come from OpenCASCADE's `Font_BRepTextBuilder` (FreeType, already linked)
  with one bundled TTF in `deps/fonts/`; a font that is not there is refused,
  naming those that are.
- **Single-stroke text and an Engrave operation.** Hershey glyphs (public
  domain polylines, embedded as data) give open centreline curves; Engrave
  follows open sketch curves at a depth with a V-bit, derived and verified
  like the other types. Outline text can already be cut as pockets without it.

Before building, read how the programs in `~/Documents/CAD` do it and take
what holds up: FreeCAD (ShapeString, Path Engrave and VCarve), OpenSCAD
(`text()`), SolveSpace (TrueType and its vector font), KiCad (the stroke
font), and blendercam, pycam and Kiri:Moto (grid-apps) for engraving.

Tests: a text entity's outline matches the glyph's advance and height and
survives a save; a cut label's faces are named and pickable; an unknown font
is refused; an Engrave op on single-stroke text posts a file whose moves lie
on the strokes at depth and verifies clean; the board generator engraves
a-h and 1-8.

## 6. Looking back

After 4 and 5: where the time in this round went, what blocked, and what in
the build, test and development system should change. Read from the session
transcripts, git history and the agents' reports, not from memory: builds
(compiles killed for memory, header rebuilds, ninja contention between
worktrees), test runs and their waits, agents (duration, rework, merge
conflicts), and failures that turned out to be the environment. The output
is a list of changes with their expected saving, each small enough to act on.

## Verification

Each phase's tests as above, the headless and GUI suites, the CAM-off build,
and the chess generator rebuilt from nothing after phase 4 with
every `cam_verify` read. At the machine: calipers on the first pawn.
