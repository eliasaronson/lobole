# The 4th axis — what is there, what it costs, and what to decide first

Indexed 4-axis shipped at C9/v67: a setup names an A, the part is
counter-rotated once, the 2.5D pipeline runs unchanged, and the post writes
one `A` word before the first cut. This plan is about the other half —
**continuous** 4-axis, where A moves while cutting — and about the three
decisions that have to be made before a line of strategy code is worth
writing.

Surveyed first (`~/Documents/CAD/CAM`), because the last CAM round's survey
changed the design and this one changes the order.

## What the survey says

| | FreeCAD CAM | Kiri:Moto | blendercam | CAMotics | opencamlib / pycam |
|---|---|---|---|---|---|
| indexed | `rotation.py`, 830 lines | `op-index.js`, **47 lines** | wired, disabled in the dispatch table | — | — |
| continuous | 2,324 lines, 3 patterns | `topo4.js` + `op-lathe.js`, 729 | 278 + a 253-line sampler | — | — |
| representation | an `A` param on a `Path.Command` — already in its ADR before rotary existed, so **zero** | an optional `.a` on `Point`, **~8 lines** | a parallel `rotations[]` array, stored as a Blender shape key, un-rotated at post time | a 9-axis vector whose geometry drops to XYZ | — |
| feed compensation | **yes** | no | no | no | — |
| rotary verification | **none** | **yes**, CSG | no, and not gated off | — | — |

Three things generalise, and all three are about us.

**1. The representation is nearly free; the strategy is not.** Kiri:Moto's
entire rotary data model is one setter and three lines of export. FreeCAD's
was in its internal-representation ADR before any rotary strategy existed.
Both then spent 700–2,300 lines on the strategies. We are already on the
right side of this: `Move::hasA`/`Move::a` exist and the post writes them,
and `DropRequest::frame` is the parameter a rotary drop cutter needs.

> As built it took a different door, and `DropRequest::frame` is still
> unused. A rotary drop is the dilation of the radius field the verifier
> already samples (`RotaryCL`), so it costs one mesh instead of one per
> angular step, and it shares the grid with the thing that checks it. The
> point the survey was making held — the seam was in the right place — but
> the cheaper seam was the MAP, not the frame.

**2. Only FreeCAD compensates the feed, and not the way a controller would.**
`_FeedClamp` scales `F = f · 360 / (2πr)` for constant surface speed, floors
the radius at `r_safe = max(r, 1e-9)`, clamps at a user `MaxFeed` and reports
once per operation how often it clamped. Nobody emits `G93` inverse-time —
CAMotics implements the mode in its interpreter and no generator emits it.
Kiri:Moto and blendercam emit the naive `F`, which is wrong near the axis,
and neither says so.

**3. A height-map simulator cannot be rescued, and every project knows it.**
They differ only in the response. FreeCAD left its height map alone and wrote
1,873 lines of G-code regression tests instead. Kiri:Moto wrote a *second*
simulator for the rotary case — a Manifold CSG solid with the tool rotated
into the part's frame, arc-length-correct. blendercam left its 2-D height map
reachable from a 4-axis path with no guard, which is the one outright bug the
survey turned up.

That third row is the whole problem for us, because our height map is not a
side feature. **One height map feeds both the picture and the verdict** — that
is the C4/C5 rule this subsystem is built on, and it is why "max gouge 20 µm"
is a number and not an impression. Continuous 4-axis breaks it.

## The three decisions, in order

### D1. What does the verifier do when A moves?

Three honest answers, and only three:

- **Refuse.** Say the file is rotary and unverifiable here. Shipped already —
  the parser names a moving A rather than dropping it, and it counts against
  `ok`. This is the floor, not an answer.
- **A second simulator**, Kiri:Moto's answer: a solid model the tool is
  rotated into. Correct, and a dependency plus a second engine that can
  disagree with the first — the exact split `simulation.md` rejected when it
  chose one height map over FreeCAD's two.
- **A cylindrical height map**: `r(x, θ)` instead of `z(x, y)`, the stock as a
  radius field about the rotary axis. Same code shape, same `TipHeight`, same
  `Compare`; it cannot represent a part that is not roughly a body of
  revolution about the A axis, and it cannot represent the flats an indexed
  job cuts. FreeCAD's rotary ops refuse anything but cylinder stock for
  exactly this reason, and refusing the same way is defensible.

**Recommendation: the cylindrical map, with the refusal above it as the
gate** (BUILT — see [As built](#as-built)). It keeps the one-engine rule, it
reuses `CarveSweep`/`SampleModel`/
`Compare` almost verbatim, and its limit — round stock, one rotary axis — is
the same limit every surveyed project imposed on its rotary ops anyway. This
is the decision to take first because it decides whether continuous 4-axis is
a thing this app can honestly claim at all.

### D2. What does a feed mean on a rotary move?

The memory from the surfacing round says *decide the feed model before the
rotary representation*, and it was right. Today `CuttingMinutes` measures
Cartesian length and `continue`s on a zero-length move, so a pure-A move is
free: a continuous program's cycle time would be understated silently, and the
playback timeline — which is path distance — would jump.

FreeCAD's answer is the only complete one in the survey and it is cheap: scale
`F` for constant surface speed, floor the radius, clamp at a stated maximum,
and report the clamping once per operation rather than per move. Take it, and
take the reporting with it, because a clamped feed that nobody is told about
is a cut running at the wrong speed.

> As built it kept the clamp and the reporting and not the formula.
> `360 / 2πr` is right only for a pure turn; a move that turns A while X
> advances is fed by whatever the CONTROLLER measures, and Mach3 and LinuxCNC
> measure it differently. The post scales by that dialect's length over the
> tip's ([rotary-continuous-plan.md](rotary-continuous-plan.md), S3).

### D3. What is the stock?

`StockMode` is `Relative | Fixed`, both boxes. A rotary job's blank is a
cylinder on centres. Round stock is named as deferred in both `plan.md` and
`surfacing.md`, and D1's cylindrical map needs it: the map IS the stock.

## What to do before any of that

Four things, all small, all worth doing whether or not continuous is ever
built. Three are already done, and are listed because they were found by
auditing this road rather than by anything failing.

1. ~~Arc fitting is off for every indexed job~~ — fixed (`3e3bc097`).
2. ~~`rotation_a` accepted without a 4th axis~~ — refused (`3e3bc097`).
3. ~~The parser drops an A word~~ — named, and the parser's findings now reach
   the verdict at all (`8efffb81`).
4. ~~`ClipToMachine` has no test~~ — `tests/test_cam_travel.cpp` (`7bbbb80c`),
   and it found one: an inserted retract or re-entry lost the A of the path
   it was inserted into.

And two structural ones, in the order the code asks for them:

- ~~**`CamVerify.cpp` is two subjects**~~ — split at `8286171a`. The parser is
  `CamGcode.{h,cpp}` (278 lines) and depends on nothing but `Setup`; 860
  lines of map, carve, compare and verdict are left, which is one subject.
- **`CamStrategies.cpp` grew 31% in two weeks** (1,068 → 1,399) and is the
  largest file in the subsystem. It holds every generator plus the clip
  epilogue plus the rotary stamp. Still true, and the rotary strategy has now
  landed in it — so the seam is owed rather than optional.

## What is NOT in the way

Worth saying, because it is the part that usually is. The generation side is
in good shape: the drop cutter already takes a `frame` and refuses a rotated
one by name rather than silently dropping along the wrong axis; the surfacing
patterns take plain values and return polylines; toolpaths are generated in
setup space and have been since C1. That is the structure FreeCAD credits for
its rotary work costing 42% of its 3-axis surfacing, and we have it already.

## As built

D1 shipped: `CamRotary.{h,cpp}`, `RadiusMap`, and `VerifyGcode` routing
itself. What is worth recording.

**The router reads the FILE, not the setup.** A program whose A moves is cut
from a cylinder that turns; one whose A does not is cut from a block that
does not. Asking the setup would get the case that matters wrong — a
rotary-capable machine posting a 3-axis job — and the user's own 6090 profile
declares `has_rotary_axis: true`, so that case is not hypothetical. Reliefs
and tabs are 2.5D statements about a contour and are not carried into a
rotary check rather than being silently ignored inside it.

**The angular grid is derived from the arc, not typed as an angle.** Enough
cells that one cell's arc *at the blank's own surface* is about `res`. A
typed step is right at one diameter and wrong at every other: it would
measure a 200 mm blank four times more coarsely than a 50 mm one and say
nothing about it.

**A cell that straddles a step is measured against its neighbours.** The
sampler takes the furthest point of the part in each cell, exactly as the
height map takes the highest. At a shoulder — a 5 mm step on a
half-millimetre grid — the boundary cell reads as the big diameter while a
6 mm cutter, which does not stop at a cell edge, legitimately turns it down
to the small one. Cell to cell that is **5 mm of "gouge" on a correct
program**, and a verifier that cries wolf at every shoulder is one nobody
reads. Gouge is measured against the smallest designed radius in a cell's
neighbourhood and leftover against the largest, which costs one cell of
resolution at a vertical face — precisely what the grid cannot resolve there
anyway.

**One count per triangle EDGE, not one per triangle.** A cylinder's side
meshes as 60 mm by a fraction of one. A single sample count from the longest
edge either samples the short direction absurdly finely or, once clamped,
leaves whole cells between samples along the long one — a plain bar sampled
as a radius of **zero** in places, which the control case caught on the first
run.

**And the bug the split created.** Recording the A word meant writing it onto
every emitted move, and the arc re-expansion walks a local angle it also
called `a`. It shadowed the modal A word, so every chord of every arc carried
the arc's own parameter as its rotary axis — an ordinary indexed job then
read as a trunnion sweeping from −π to π, which routed it to the rotary stock
model and reported 36 mm of gouge on a correct program. The C9 indexed-frame
test caught it in one run, which is the argument for that test existing.

### Still open

- ~~**D2, the feed model.**~~ Done, both halves. The verifier's: a rotary
  move has length -- the arc the tool tip travels, combined with the linear
  part. The generator's landed in the POST rather than the strategy, and not
  as FreeCAD's constant-surface-speed scale, which is right only for a pure
  turn: a cutting move that turns A is posted at `F = f · L_controller /
  L_tip`, where the controller's length is what the dialect measures (mach3
  the combined vector with degrees as millimetres; linuxcnc the XYZ length
  when there is one, else degrees). Clamped at the machine's `rapid_feed` and
  reported once per operation; the cycle time is clocked the controller's way
  too. See [rotary-continuous-plan.md](rotary-continuous-plan.md), S3.
- **D3, round stock.** The blank is DERIVED from the part (its extent along
  the axis, its furthest point from it, plus the setup's margins — the radial
  one taken as the larger of Y and Z, because a box's margins are all a
  `Stock` can state). A round stock mode is the honest fix and is a format
  change.
- ~~**Nothing generates a rotary toolpath.**~~ `surface_rotary` does (v122),
  and the order held — the check existed before the generator, so the
  generator was trustworthy from its first line. At v122 it was a raster:
  passes along X at one A each, the part turning only between them, which is
  not continuous 4-axis. v123 adds `rotary_pattern: spiral`, one helix with A
  turning while the tool cuts, and roughing layers from `stepdown`
  ([rotary-continuous-plan.md](rotary-continuous-plan.md)).
- ~~Rapids are not checked against the rotary stock.~~ Done. One walk serves
  both cutting and asking, so the two cannot come to disagree about what the
  tool removes -- a rapid is tested against the stock and never carves it.
- ~~**No picture.**~~ Built — see "The picture, as built" below. It was:
  `CamPlayback` draws a height map. A radius map had no
  renderer, so a rotary file verified but could not be watched — and it is
  REFUSED rather than drawn wrong (`Timeline::unsupported`), because the
  first thing the dogfood asked of it was to simulate the handle and it did,
  reporting 5,004 mm³ removed against about 77,000 actually cut.

  The BACKPLOT is drawn, and had the same fault in a quieter form: setup
  space is the MACHINE's frame, where a rotary job's tool never leaves the
  plane y = 0, so the pin's passes were drawn lying on top of each other in
  one flat fan along the top of the blank. True about the machine, useless
  about the part. A move carrying an A is now drawn where it lands on the
  WORK — rotated back by its own angle — so the passes wrap the part as the
  flutes they cut.

  And the PANEL's verdict was routed the way `VerifyGcode` routes: it called
  the planar `Inspect` directly (it also wants the Inspection for the
  leftover overlay) and so read the pin off a height map — "Cuts 3.98 mm
  INTO the part, that is scrap", "20 rapids through material", beside one
  quiet line admitting the file could not be read. Three confident wrong
  numbers and one true one is worse than no verdict.

## The picture: simulating a part that turns

Decided: **the part turns** — the tool stays where the machine keeps it, over
the axis, and the part, the stock and everything drawn on them rotate by the
cursor's A. That is what the 6090 will do, so it is what the picture should
show.

It is display and transport, not simulation math — the same thing
simulation.md said of the planar simulator — because the math is built:
`RadiusMap`, `CarveSweepRotary` and the blank derivation are the verifier's.
Carving with them is what keeps the one-engine rule: the last frame of a
rotary playback is the stock `InspectRotary` reads its verdict from.

**Timeline.** A is interpolated along a segment like X, Y and Z, and a pure-A
move gets a length — the arc the tool tip travels, the same definition
`CuttingMinutes` uses — or the slider would jump across every rotation.

**The stock while scrubbing.** `RotaryStockSim`, the twin of `StockSim` over a
radius field. Forward is incremental; backward cannot un-cut, and the planar
header already says what to do when replaying from the blank is too slow:
keep snapshots. For the pin it is too slow — 644,160 moves at 65 µs is a
42-second replay from the end. Snapshots every K segments, K DERIVED from a
memory budget rather than typed, bound any backward seek.

**The mesh.** A tube over the grid, `(x, r·cosθ, r·sinθ)`, with normals from
the surface's own partial derivatives and caps at both ends, built in the
PART's frame. Gated by the existing "every triangle faces the way its normals
do" test.

**Turning it.** One matrix, `R_x(A)`, on everything attached to the part: the
ghosted bodies and their outlines (per-instance transforms, identity on a job
tab because the geometry is already placed), and the stock (its push constant
has always carried a model matrix and the shader already applies it to
normals). The tool is in machine space already and does not move with it.
Everything returns to identity when playback ends.

**The path.** Drawn in MACHINE space during rotary playback — a window of
moves around the cursor. In machine space every pass lies along the top of
the axis, which is exactly where the tool is; the passes already cut show as
grooves in the stock, which is the better record of them.

### The picture, as built

`a_rotary_part_turns_as_it_plays` reads the renderer's OWN instance state:
at A = −88° a point at +Y on the part lands at (0, 0.035, −0.999), exactly
where R_x(−88°) puts it; 179 frames while playing at 20x, worst 18 ms; and
the part stands still again once the playback ends.

On the rolling pin itself, through `cam_simulate`: removed volume climbs
through the program to 156,712 mm³ against ~163,000 for a Ø54 blank down to
the 524 cm³ pin, A turns with the cursor, and **scrubbing back from the end
to 10% takes no measurable time** — it restores a snapshot, where a replay
from the blank would have been 42 seconds.

Three things the build found:

**The one-engine rule held bit for bit, because the blank was shared first.**
`MakeRotaryBlank` came out of `InspectRotary` so the simulator and the
verifier derive the stock ONCE; two derivations would each be right and drift
apart. With that, playing to the end leaves a radius field identical to the
verifier's cell for cell — tested as an exact equality, not a tolerance.

**A snapshot is only correct on its FIRST visit.** It must hold "every
segment before this one carved, and this one untouched". The carve walks
segments in order and re-carves a partly-cut one from its own start, so the
first arrival at a snapshot boundary always has that property — and a later
arrival (from a backward seek) never re-takes it. Tested by scrubbing to six
cursors, whole segments and part way into them, against a fresh forward play.

**`SyncCamPlayback` crashed offscreen, and had since the 12 Hz remesh cap.**
It asked `ImGui::GetTime()` for the clock, and the offscreen path has no ImGui
context: `cam_simulate` then `screenshot` on a headless instance was a
SIGSEGV. Nobody had taken that screenshot until the rotary picture wanted
looking at. A steady clock now, and `offscreen_smoke` has the phase that
catches it — checked by putting the old line back, which fails the smoke.

