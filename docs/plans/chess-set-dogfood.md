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

## 5. Text (later)

A text sketch entity (a single-stroke font for engraving, an outline font for
pockets) and an engrave operation: the board's a-h and 1-8.

## Verification

Each phase's tests as above, the headless and GUI suites, the CAM-off build,
and the chess generator rebuilt from nothing after phase 4 with
every `cam_verify` read. At the machine: calipers on the first pawn.
