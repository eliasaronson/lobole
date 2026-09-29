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

The board's a-h and 1-8, engraved. Two layers, built in parallel once the
entity's shape below is fixed: a text sketch entity (part side), and an
Engrave operation that follows open sketch curves (CAM side).

### The text entity

`Text` in the `sketch::Entity` variant (`SketchFeature.h`), JSON
`type: "text"`, modelled on `Ellipse`: one entity, rigid geometry, one free
point.

| field | meaning |
|---|---|
| `text` | UTF-8, one line (a newline is refused) |
| `font` | a bundled font by name: `sans` (outline), `hershey` (single stroke); an unknown name is refused, listing the known |
| `height` | CAP height, mm, an expression -- what every other program means by "size" in four different ways (em in OCC, em x 100/72 in OpenSCAD, line height in FreeCAD's FT2FC); measured once per font from 'H' |
| `anchor` | UV point, the solver's only unknown pair (`PointRef 0`) |
| `angle` | radians stored, degrees shown, an expression |
| `halign` / `valign` | `left|center|right`; `baseline|bottom|middle|top` from the FONT's metrics, never the string's ink, so a row of labels lines up |
| `spacing` | extra tracking, mm |
| `construction` | as every entity |

The glyph geometry is never saved: it is regenerated from those fields
(format bump). The label's box corners are `PointRef 1..4`, readable by the
solver but not moved by it, so a label can be dimensioned from its edge.

- **Outline glyphs** from OCC's `StdPrs_BRepFont` (FreeType), Init'd on a file
  path from `deps/fonts/` -- never `Font_FontMgr` name lookup. OCC's
  unicode-subset fallback is switched OFF (it loads missing glyphs from a
  SYSTEM font, so the same document builds differently on two machines), and
  every character is checked with `HasSymbol` first: a missing glyph is
  refused, naming it. `TKService` is linked explicitly (today it arrives only
  transitively). Composite-curve mode stays off, so each segment is a line or
  a 3/4-pole Bezier, which IS the sketch's clamped B-spline of degree 2/3: no
  flattening. Glyphs are placed by their advance (kerning from the `kern`
  table; no shaping, said in the docs), each translated by us rather than
  through `Perform`, which shares one cached TShape per repeated letter.
- **The bundled outline font** has no overlapping contours (profiles refuse
  partial overlap, rightly): Liberation Sans or DejaVu Sans, vetted by a test
  that builds every ASCII glyph into loops. Its licence goes in `LICENSES/`.
- **Single stroke:** the original Hershey Roman Simplex (`romans.jhf`, public
  domain with its attribution shipped, `LICENSES/Hershey.txt`) embedded as
  data; one decoder reads the `'R'`-offset pair encoding (Y points down).
  Stroke text is OPEN curves: profiles skip it silently (an extrude that
  picks it is refused by name), and Engrave follows it.
- **Profiles:** glyphs are just loops -- containment depth already makes a
  letter's counter a hole. The stale "largest loop is outer" comment
  (`SketchProfile.h`) and the sketching doc are corrected.
- **Naming:** a `LoopEdge` gains a sub-index (glyph, contour, segment), glyph
  counted over RENDERED characters only; side faces are
  `<feature>/g<textId>.<glyph>.<contour>.<seg>`. Picking any glyph's region
  picks the whole label (as a pattern's siblings), and CAM targets the entity
  (`g<textId>`), which survives editing the string.

### Engrave

`OpType::Engrave` (`engrave`): follows open or closed SKETCH curves -- a
stroke label, or any line/arc/spline -- ON the curve, not offset. Target
`{feature_id: <sketch feature>, element: "g<entityId>"}`, resolved through
the sketch plane, the job part's placement and the setup frame; a plane that
is not facing the spindle is refused, naming its angle.

- **Depth from width:** `width_mm` (groove width at the surface) or
  `depth_mm`. For a V-bit `depth = TipHeight(tool, width/2)` -- the verifier's
  own function, so the two cannot disagree; a width past the cone is refused.
  A flat or ball tool takes a depth only. The default width derives from the
  text height (cap/8), never a literal.
- **Z:** the top is the sketch plane in the setup (the surface), probed along
  the strokes -- text sketched off the surface is refused, not air-cut.
- **Order:** nearest neighbour over strokes, reversing an open one when its
  far end is nearer, then 2-opt; a stroke that starts where the last ended
  stays down. Entry is a straight plunge at the plunge feed (a V-bit centre
  cuts; a 3-degree ramp is longer than most strokes); hops between strokes at
  a short retract, clearance between labels.
- **Verification:** the toolpath says what it cut on purpose (as reliefs and
  tabs do): its nominal groove -- the strokes swept by the cone to depth --
  and a cut inside it is excused as engraving, while one outside it is a
  gouge. A groove narrower than three cells warns.

Tests: a label's cap height and advance match the font's; it survives a
save; a cut label's faces are named and picking one picks all; an unknown
font and a missing glyph are refused; overlapping-contour check over the
bundled font's ASCII; an Engrave op on stroke text posts moves on the strokes
at depth, reverses strokes where that is shorter (against a fixed-order
control) and verifies clean; the board generator engraves a-h and 1-8.

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
