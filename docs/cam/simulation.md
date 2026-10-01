# Verification and the cutting simulation

A wrong FEM number misleads you; wrong G-code breaks a tool, a spindle or a
hand. So the posted file is checked, headless and automatically, and the
picture a person scrubs through is the same simulation the check reads.

Code: `src/core/cam/CamGcode.{h,cpp}` (the parser), `CamVerify.{h,cpp}` (the
height map, the carve, the compare, the verdict and its sentences),
`CamRotary.{h,cpp}` (the rotary twin, [rotary.md](rotary.md)),
`CamFeeds.{h,cpp}` (cutter findings), `CamSim.{h,cpp}` (timeline and
playback stock), `CamStockMesh.{h,cpp}` (meshes), `src/render/StockRenderer`,
`src/ui/CamPlayback.{h,cpp}`, `src/ui/CamTool.cpp` (the panel's verification).

## One engine

- **It runs on the posted file.** `ParseGcode` reads the program back —
  modal state, `F` (a non-positive feed is reported, not adopted), `T`, `M6`,
  `G2`/`G3` as chords, a modal A, `G92 A`, and the `(op N)` comments that
  attribute moves to operations (only exactly `op <digits>`, so a
  `SKIPPED op` line does not start one). `ProgramInSetupSpace` adds the work
  origin back. So a bug in the post, in modal state or in linking is seen,
  and so is a rapid through material that leaves the part perfect.
- **The stock is a height map** `z(x, y)` over the stock box (`MakeStock`),
  and a tool is its profile function `TipHeight(tool, r)` — flat, ball, V and
  drill from one function. `CarveSweep(from, to, tool, stock)` tests each
  cell analytically against the swept segment (exact, not oversampled),
  lowering it to the lowest the tool reaches over the stretch of the move
  during which the cell lies under it, not to its height at the closest
  approach. That stretch is an interval of the move: a flat end is lowest at
  one of its ends, and a shaped tip is sampled across it. At the closest
  approach, a descending move left cells it went on to cut up to its whole
  descent too high, which read as leftover on ramps and as rapids through
  material when the tool lifted out. It returns the volume it took (what
  [the bite](#bite) is measured from), summed over its cells as integers in
  units of 2^-30 mm so that a volume taken band by band is the same number
  ([On every core](#on-every-core)), and is the one carve: `Simulate`,
  `Inspect` and the playback all call it, carving
  each segment from its own start so a program played in pieces ends
  bit-identical to one played whole.
- **The designed surface** is sampled on the same grid (`SampleModel`, a
  flat-tool drop cutter). It meshes the model detached
  (`GenerateMeshDetached`) at a quarter of a cell, because the default mesh
  deflection scales with the part and would be coarser than the grid
  measuring it, and because meshing a shared shape would re-mesh the
  document behind the renderer's back. Each triangle is handed to the bands
  of rows it covers and the bands are sampled on every core; a cell is the
  highest any triangle stands over it, so the map does not depend on the
  count. By rows rather than by triangles, because a planar part's faces
  mesh as a few triangles hundreds of millimetres across.
- **One simulator, not two.** The picture comes from the same map, fed by the
  same posted file, as the verdict: `StockSim::SeekTo(End())` equals
  `Inspect`'s machined map bit for bit (a test), so every pixel on screen is a
  number the panel can also state.

### The cutter's wall

A cell is within the cutter's reach when its centre is at most
`R - kCutterWallTolerance` from the tool's axis (1e-4 mm, `CamVerify.h`),
not `R`. Toolpaths lie on the grid's own lattice, so a 6 mm cutter on a
0.25 mm grid puts its wall exactly on cell centres, and whether those cells
were inside was decided by the last bit of rounding of the stock's origin.
Moving the wall a tenth of a micron inward takes it off the lattice: a cell
on the wall is outside by a decision. The verdict thresholds are around
0.01 mm, a hundred times larger, so no real answer moves.

The same reach is used by every test that asks whether a cell is under the
tool: `CarveSweep`, the rapid check, the coverage that decides where leftover
counts (`touched`) and the nominal engraved grooves. The rotary walk uses it
too ([rotary.md](rotary.md#the-cutters-wall)). The pin is `cam verify: a cell
on the cutter's wall is outside by a decision, not by rounding`: a plunge and
a traverse on the lattice carve the same metal with the stock's x0 nudged by
±1e-12 mm.

### On every core

The planar check runs on every core, and its answer is the one-thread answer
bit for bit whatever the band or thread count (`cam verify: the check on
every core is the serial check to the bit`: rapids through stock, bites,
plunges, the shank, the holder, a clamp, two tools).

- **The walk over the program** (`Inspect`) splits the map into **bands of
  rows**, as the rotary walk splits its map along the axis
  ([rotary.md](rotary.md#on-every-core)). Each band walks every move in file
  order over its own rows only. A cut lowers a cell to the lowest the tool
  reaches, so the final map does not depend on the order, but a rapid, the
  shank and the holder are checked against the stock as it stands at that
  line, and each cell still sees exactly the serial sequence. No two
  workers write a cell.
- **What a band finds is merged per move, in band order.** A hit (rapid,
  shank, holder) is the deepest any band found, a tie going to the lowest
  row as the serial scan's did; the count, first line and worst line and
  place are then read in file order. A move's removed volume and a plunge's
  bite are sums over cells, taken as integers, so the bands' shares add to
  the serial number exactly, and the bite windows are judged from them in
  file order afterwards. Fixture hits read only the fixtures' own field,
  never the stock, so each move is tested on its own.
- **Bands outnumber the workers** (four per worker, none under four rows),
  because the cutting is uneven over a part; a handful of moves stays on one
  thread. `ScopedInspectBands` pins the count for a test.
- **The compare** (`Compare`) runs in bands of rows too, each keeping its own
  worsts, merged in band order; the area another setup cuts is counted in
  cells. `SeedFromEarlier` walks its columns on every core.

A leftover cell asks the other setups whether they remove its excess
(`OtherSetups::Claims`), and on the chess board's face after the epoxy that
is half of its four million cells at 0.25 mm: the pockets the top setup cut
for the epoxy stand in the face setup's model as 3 mm of excess. Taking each
sample of each column through two 4x4 maps into model space and out, with a
count allocated per cell, cost 330 ms of that check (21 ms on the top
setup, which asks 1,761 times). The column's ends are now mapped into each
setup's frame once, and a column that stays in one cell of a height map (a
face-up and a flipped setup of one block) is decided from its two ends: a
sample is removed when its z lies in one interval, and z is monotone along
the column. That check's compare is 27 ms now.

On the chess board, on 22 threads, a whole check (the other setup read,
then this one) went from 0.94 s to 0.36 s on the top setup at 0.25 mm and
from 1.35 s to 0.35 s on the face after the epoxy; at the panel's 0.4 mm
from 0.47 s to 0.22 s and 0.56 s to 0.23 s. With the other setup read back
([below](#each-other-setup-inspected-once)) it is 0.10 to 0.18 s, of which
about 40 ms is meshing the part for `SampleModel`. The chess set's CAM
(`generators/chess/run_all.sh PORT --from cam`) went from 5 min 02 s to
3 min 07 s with every verdict the same.

## The verifier

`Inspect` (and `VerifyGcode`, which routes a file whose A moves to
`InspectRotary`) compares the machined map with the designed one.

- **Resolution.** `cam_verify` defaults to 0.25 mm cells (`resolution`); the
  panel verifies at 0.4 mm, on a `cam-verify` background job, and its
  tolerance is one and a half cells — a "gouge" of half a cell is
  quantisation.
- **Neighbour rule.** A cell is gouged only if it is below everything its
  neighbours were designed to be, and left over only if above all of them, so
  a cell centre landing on an island's corner does not read as the island's
  whole height, and two grids of different resolution agree.
- **Leftover only where the tool went** (`Inspection::touched`): a job that
  clears one pocket in a block leaves the rest of the block correctly. A
  program that machines nothing at all shows as `untouchedFraction`.
- **Below the stock** (`below_stock_mm`) is cutting into the spoilboard,
  which is what a through cut is for; it is reported and never counted as
  gouge.
- **Offcuts are not leftover**, and pieces nothing holds are said
  ([below](#tabs-and-offcuts)).
- **Told, not guessed:** each toolpath's corner reliefs are skipped for gouge,
  and its tabs are excused for leftover up to their own height (a relief
  excuses a cut too deep, a tab one that did not happen, so a tab needs its
  height or it would excuse a genuinely uncut slab above it). An engraving's
  grooves excuse a cut down to their own shape and no further
  ([below](#engraved-grooves)).
- **Rapids through material** are counted with the worst depth, place and
  line (`rapids_through_stock`, `worst_rapid_depth_mm/_at/_line`: a graze and
  a crash are the same count and different numbers). The rapid check models
  the tool by `TipHeight` like the cut does, so a drill retracting up its own
  hole does not find its own cone in the way. A rapid never carves.
- **File errors and warnings.** What the parser could not read (a NaN
  coordinate, a zero feed) counts against `ok`; what a controller may refuse
  though the parser survived it (a nested comment) is a warning.

### Engraved grooves

An engraving cuts INTO the part on purpose, and the model does not have the
groove, so each stroke is declared (`Toolpath::Groove`: its polyline, the
height its tip ran at, the surface and the tool) and the verifier rasters the
NOMINAL groove onto its grid: per cell, the stroke swept by the tool's own
shape, `tip + TipHeight(tool, d)`, the same function the carve uses, so a
correct engraving carves each cell to exactly that. A cell's gouge is measured
against the lower of the model and the groove there: a cut inside the groove
is engraving, a cut below it (an engraving deeper than it declared) or beside
it (one that ran off its stroke) is gouge. Unlike a relief, which excuses any
depth inside its bite, a groove excuses its own shape and nothing else.

The other direction is measured too: `grooveShallow`
(`groove_shallow_mm`) is how far above its groove the cut stopped, at the
worst cell where the groove is below the model, and it counts against
`complete`. A groove narrower than three cells of the check is one the grid
barely resolves (its floor falls between cell centres), and the verdict counts
those (`narrow_grooves`) and says to verify finer. Grooves are 2.5D
statements like reliefs and tabs: a file that turns while cutting is checked
by the rotary inspection, which reads none of them, and an operation cut at
several indices does not carry them.

### Threads

A height map cannot see a flank that faces sideways. A thread mill's head
seen from below is flat to its neck and then its lower flank
(`TipHeight`), so the carve clears the whole column the head sweeps -- the
crests between the grooves included -- down to where that flank ran. Judged
as a surface, every milled thread would be a ring of gouge as deep as the
plate; excused blindly, a helix at the wrong radius or pitch would pass. So a
thread is judged ON ITS OWN TERMS, from the posted file, against what the
toolpath declared (`Toolpath::Thread`: its centre, side and hand, pitch, the
class and its pitch-diameter band, the span, the bore, the tool), in
`JudgeThread` and `MeasureBoreBefore` (`CamThreadMill.cpp`):

- **The helix.** The operation's moves (by the file's `(op N)`) are grouped
  into runs that keep one radius round the thread's centre while Z moves --
  the arcs re-expanded to chords by the parser -- and a run of at least a
  half turn is a pass. The last pass (the widest inside, the narrowest
  outside) gives the **pitch** (Z per turn), the **hand** (turning
  counter-clockwise while rising is right-handed) and the **pitch diameter**
  its tip reached: the rim at the pass's radius plus the cutter's radius,
  the V's point a flat's reach past that, and PD = 2 x point - H inside
  (+ H outside). That has to lie in the class's band.
- **The span.** The V's middle ran over the last pass's Z range plus half
  the head's width; full thread is from half a pitch inside each end of
  that. Short of the span the thread allows (`fullLo`/`fullHi` of the
  declaration: a blind floor keeps the head from the last half pitch and
  half a V) is `short_mm`.
- **The bore.** The map as the program leaves it when the thread STARTS:
  the starting stock, carved by every move before the op's first, over the
  bore inside the tap drill's wall (round the boss, outside). Stock standing
  there above the head's lowest tip -- never below the stock's bottom, where
  the spoilboard is -- is `bore_standing_mm`: the tap drill was not cut first,
  and the head would plunge into metal.
- **The neck.** How far the neck stays off the bore's wall at the last
  pass, `neck_clearance_mm`; negative, it rubs the crests.

The carve itself still runs, so everything else is judged as for any cut: a
cell inside the widest tip radius the class allows (outside a boss, from the
narrowest) is excused down to the head's lowest tip in the file, and
nothing else -- a helix too wide shows as gouge beyond it, a head below it as
gouge under it, the holder and the shank are probed on every move.
`cam_verify` answers `threads`, one entry per thread (`ThreadVerdict`), and
`Explain` says a line per thread. A bore not cut and a neck that rubs count
against `ok`; a thread not judged (no helix of it in the file, or a file
that turns A), out of its band, of the wrong pitch or hand, or short count
against `complete`.

### Tolerance bands

A toleranced round wall the program aimed at its band's middle
([operations.md](operations.md#tolerance-bands)) is judged against the
band, not nominal: a grid of tenths of a millimetre cannot see a K7's 25
microns. From the posted file (`JudgeFits`, `CamFindings.cpp`), every
cutting move of the operations that declared the wall, within the wall's Z
span, gives its rim's distance from the wall's centre -- the move's own tool
from the setup's table, a cut's endpoints lying on its arc -- and the widest
round a bore (the narrowest round a boss) is the diameter the file cuts.
Walls declared by several operations (a bore and its spring pass) are one
wall. `cam_verify` answers `fits` (`FitVerdict`); `Explain` says a line
each. Past the band on the side nothing puts back -- a bore too big, a boss
too small -- is scrap and counts against `ok`; short of it is metal left on
the wall and counts against `complete`, and so does a wall no move of its
operations reaches. The band's ring, a cell either side, is excused from
gouge (down to the wall's bottom) and from leftover (up to its top), so a
band wider than the grid's quantisation reads as neither.

### Tabs and offcuts

A height map cannot watch a slug drop out of a through cut, so the machined
map is split, after the cut, into the pieces of material that through cuts
separate (`OffcutCells`): a cell cut to the stock's floor separates, and so
does a tab's own material in the kerf, which holds a piece rather than
joining it to its neighbour; pieces are 8-connected, so a kerf separates
only where it is cut through cell to cell.

- A piece with none of the part in it is an **offcut** -- the slug of an
  opening, the frame round a part cut from a sheet -- and is never
  leftover, wherever the tool touched it (a lead arc into the slug, a ramp
  in the frame). `offcut_area_mm2` says how much. A profile on tabs
  verifies complete.
- A piece held neither by the stock's edge (the sheet, clamped) nor by a
  tab is **cut free**: an offcut or the part itself, and it would fly.
  `cut_free` lists them (where, how big, whether it is the part), the part
  first, and `Explain` says each as a warning. A finding, not a failure: a
  vacuum table, screws or tape the job does not model may hold it.
- A setup that cuts nothing through is one piece, and nothing here changes.

### What the job does not cut

`cam_verify` lists every hole and thread of the job's parts that the job
does not cut (`uncut`, `UncutFeature`), and `Explain` says one line per
feature. Without it a forgotten hole was silent: leftover is measured only
where the tool went.

- **A hole** (each bore a Hole drilled, its `HoleMark`, a pattern's copies
  included) is cut when some
  setup leaves its bore in air at three points down its axis: this setup's
  machined map where the bore stands up in it (`FindUncutHoles`), or any
  other setup's final map (`SetupStock::Removes`). A hole cut by another
  setup is not listed; one no setup reaches is.
- **A thread** (each `ThreadMark`, the last on a face) is cut when an
  enabled thread mill of the job targets its face and generated a thread
  (`AddUncutThreads`, from every setup's toolpaths). The height map cannot
  see a thread, so it is the operation's word.
- **A finding, not a failure**: neither `ok` nor `complete` reads it. A job
  routinely leaves a feature to another process -- the pan/tilt base
  plate's M4 threads are tapped by hand -- and `complete` stays the
  program's statement about what it set out to cut. A caller that requires
  a feature gates on the list itself, as `generators/pantilt/cam.py` does:
  anything uncut but the M4 threads stops it.

### Leftover another setup removes

A setup verified from its own fresh stock reports a flip job's half-depth
outline that its back finishes, and an indexed setup the material a turn
removes, as leftover: the same sentence as a forgotten operation. So the verifier asks the job's other setups (`OtherSetupsOf`,
CamOtherSetups.cpp):
- **What each other setup leaves:** its posted file is inspected as
  `VerifyGcode` would. Its final map says where it leaves air: above a height
  map, beyond a radius map.
- **How a point is mapped:** from this setup's frame to model space and on
  to that setup's frame. The map is `BodyToSetup`, the one transform
  `BodiesInSetupFrame` applies.
- **What counts as removed:** a leftover cell's excess, the column from the
  designed surface plus the tolerance up to the machined one, is **cut by
  another setup** when every point of it lies in air some other setup
  leaves. It is said as that setup's (`leftover_other_setups`
  [{setup, name, area_mm2}], "… mm2 left here is cut by setup 'Back'") and
  kept out of `max_leftover_mm`, the regions and `complete`. A point outside
  the other setup's stock is not claimed, so two boxes that disagree (a
  `bottom_margin` unlike `margin.z`) leave the doubtful strip as leftover.
- **What is not consulted:** a job whose parts are clamped or placed
  differently from one another has no single map to model space. It is not
  consulted, and the verdict says so (`other_setups_not_checked`), as it
  does for a setup that does not post. The panel reads the other setups
  from the pass caches and never computes them on the frame: a pass not yet
  cut is queued, the verdict names that setup as not yet compared, and it
  runs again when the passes land.
- **Where a later setup starts:** the setups EARLIER in the job (the job's
  order is the cutting order) have already cut, so a setup starts from what
  they left (`StartingStock`, `SeedFromEarlier`): each column of a planar
  setup's fresh stock is lowered, and each ray of a rotary setup's blank
  shortened, to the first point no earlier setup removed, a point counting
  as removed only 0.3 mm (or a cell) clear of what that setup left, and
  beyond a rotary setup's bar there was never any. An indexed setup after a
  turning one then starts from the turned part: checked against the fresh
  bar, every entry into the chess king's cross was a rapid 12.5 mm "through
  material" and every pass a 24 mm "bite". Rapids, bites and collisions run
  against that stock; the cells this setup never sweeps are not compared, as
  before.
- **One start for the check and the picture:** the planar check, the rotary
  check (`InspectRotary`, `StartingRotaryStock`) and the playback
  (`BeginCamPlayback`, `cam_simulate`) all start from that stock, so the
  playback of an indexed setup plays against the turned part and its last
  frame is the verifier's own stock. The playback reads the earlier setups
  from their posted files like the check; in the panel a pass of theirs not
  yet cut refuses the playback in words, as its own would, and `cam_simulate`
  generates it first. Reading a turning's file is the rotary carve of all of
  it (40 s on one core before the chess rook and bishop's mitre; it runs on
  every core now, [rotary.md](rotary.md#on-every-core)), so the panel reads on a
  worker ("Reading what the earlier setups left") and the transport opens
  when it lands, unless the file changed meanwhile; `cam_simulate` parks for
  it ([README.md](README.md#requests-that-wait)). `cam_simulate` names what
  it started from
  (`starts_from`, and `earlier_setups_not_read` for a setup that does not
  post), and the transport says "Starts from what 'Turn' left". A setup that
  cannot be read leaves its stock in place, which errs toward a finding.

#### Each other setup inspected once

What another setup leaves depends on its posted file, the setup itself, the
part and the grid, and on nothing about the setup asking. So `OtherSetupsOf`
keeps each answer (the machined map, or why it could not be read)
process-wide, under a key of exactly those: the file's text, the setup as it
is saved (`CamSetupToJson`: its framing, stock, placements, tools, machine
and name), the part's exact B-rep with the ids its bodies are placed by
(`BodiesContentKey`), and the resolution. The key is compared whole, not by
hash alone. An edit to any of them misses; where the setup stands in the
job's order is the asker's and is applied on the way out. The memo keeps
256 MB, least recently used out first. On the chess board a second check
reads the other setup back in about 3 ms instead of inspecting it again.

The setups that miss are inspected at once, each on its own copy of the
bodies (a body's bounding box is cached on the object, and two threads
would fill the same cache). Each of those inspections is itself split into
bands, and a `parallel::For` nested in another's body shares its worker's
part of the machine (`core/Parallel.h`), so the two levels divide the cores
rather than multiply the threads. `cam verify: each other setup is inspected
once, until it changes` pins the hits, the misses after an edit to the
program, the stock, the part or the grid, and two misses inspected together.

### Verdict

`ok` (`Verdict::Safe`) means nothing scraps the part or breaks the tool: no
gouge beyond tolerance, no rapid through material, nothing past the travel,
no holder, shank or fixture collision, no bite deeper than the operation's
passes, no move into the chuck or tailstock, and a file that read straight —
the flag to gate on. `complete` means nothing is left standing. They are separate
because a correct job can be `ok` and not `complete` -- a forgotten
operation scraps nothing; tabs and offcuts do not count
against `complete` ([tabs and offcuts](#tabs-and-offcuts)), a toleranced
wall cut past its band counts against `ok` and short of it against
`complete` ([tolerance bands](#tolerance-bands)), and the holes and threads
the job does not cut are listed and count against neither
([what the job does not cut](#what-the-job-does-not-cut)). A program that never touches the stock is `cuts_nothing` and not
`complete`: leftover is measured only where the tool went, so it used to read
as complete, and a setup whose operation was refused posted a file that
"verified clean". `cam_verify` also reports how much is left over and where
(`leftover_area_mm2`, `leftover_regions`, `leftover_biggest`, largest first):
several tiny regions are inside corners a round cutter cannot reach and are
correct, one large region is a missing operation. `failed_ops` names the
operations that produced no toolpath and so are not in the file (first in
`says`); they do not count against `ok`, since an empty file is safe.

`Explain` turns a verdict into the sentences the panel shows and `cam_verify`
returns as `says`, each at level Good, Warn or Bad; `MachineLines` adds the
work-area lines ([setup.md](setup.md#the-machines-work-area)). The verdict
text is asserted in tests against jobs with known faults, so the words cannot
drift from the numbers.

### Holder collisions

A tool that states both `overall_length` (tip to the collet nut's underside)
and `collar_diameter` (the nut at its widest) has its holder checked: on every
move, a cell standing above `tip + overall_length`, further from the axis than
the cutter's radius and no further than the collar's, is the nut in the work
(`FoulsHolder`, `holder_collisions` with worst depth, place and line). Blind
inside the cutter's radius on purpose: there the material is either cut or it
is a cut deeper than the flute, which the reach check already states
([setup.md](setup.md#reach)). Silent
unless both numbers are stated, and the RPC refuses one without the other —
half a holder would disable the check while looking like it enabled it. A
collar riding above the uncut stock top is skipped whole, which keeps the
check nearly free on ordinary jobs. The tool glyph draws the stated collar.

### Shank collisions

A tool that states a `shank_diameter` wider than its cutter
([setup.md](setup.md#reach)) has its shank checked on every move: a cell
further from the axis than the cutter's radius (plus one cell, where the
cutter's own wall lands on the grid) and no further than the shank's,
standing above the top of the flutes, is a wall the shank is being driven
into (`shank_collisions`, `worst_shank_depth_mm/_at/_line`). The depth alone
is not this check's business: a cut deeper than the flutes is stated at the
operation, when it is made. Silent for a tool with no stated shank, or one
no wider than its cutter.

### Bite

How deep each stretch of cutting actually bites, against what its operation
asked for. The volume a move removes, read off the height map as it is
carved, over the cutter's diameter times the distance, is the depth of a
full-width cut that would take the same. A slot at the stepdown reads as the
stepdown, and a finishing skim as a fraction of it. It is measured over
stretches of at least 2 mm (or half the diameter). A long move is judged in
pieces that long, so air on either side cannot dilute a bite. An arc posted
as short chords is averaged over enough of them that the grid's quantisation
washes out. A plunge (any move steeper than about 63°: the arc before a tab
exit ends a few microns off its point once turned into chords) is judged on
its own. It uses the mean depth under the cutter's face before it cuts,
without the face's outermost cell, where the chords' sagitta leaves a ring
standing at full height.

Profile, pocket, adaptive and face operations are measured against their
stepdown (the whole depth when it is 0), or their tab height when that is
larger, since a tab exit plunges the tab's height. A stretch more than a
quarter over that plus 0.2 mm is a finding: `over_bites`,
`worst_bite_mm/_allowed_mm/_at/_line/_op`, and `bite_by_op`, each op's
deepest with its allowance. Drilling and edge breaks are not measured, since
they plunge or skim by design, and neither are the surfacing passes, which
have no stepdown. This is what found the lead-outs that slotted a baffle's
slug full depth, and then the opening whose top pass had gone round the
recess above it. The part came out the same both times, so no other number
in the verdict saw either.

### Fixture collisions

Fixtures ([setup.md](setup.md#workholding-fixtures)) get a height field of
their own, never carved: over the union of their footprints, each cell holds
the top of the tallest fixture there (sampled with `SampleModel`) and which one
it is. On every move, rapid or cut, each fixture cell the tool can reach is
tested — against `TipHeight` inside the cutter's radius and against the
collar's underside out to the collar's radius when the tool states a holder —
with the tool's height taken where the segment passes closest to the cell, so
a ramp past a clamp is not charged for a depth it never reached there. A hit
is reported per move: `fixture_collisions`, `worst_fixture_depth_mm/_at/_line`,
`worst_fixture` (name) and `worst_fixture_by` (`cutter` | `holder`);
`fixtures_checked` counts them. The stock touching a fixture is the point of a
fixture and is not a finding. What is not checked is said: a fixture whose
part is gone, and every fixture on a rotary setup (`not_checked`). When safe Z
was raised to clear a fixture and then exceeds the Z travel, the travel line
names the fixture.

### Holding collisions

A setup held in the rotary checks every move against the chuck and the
tailstock (`holding_collisions`, the worst depth, x, line and which one); any
collision fails `ok`, and the sizes that were defaults, not measured, are
listed. [rotary.md](rotary.md#chuck-and-tailstock) has the envelope and the
test.

### Machine travel

With a work area stated, `outsideTravel` counts moves past it: X and Y against
the reach window ([setup.md](setup.md#the-machines-work-area)), each with the
first line that crosses it; Z as a span, the program's deepest point against
its highest retract. A span has no first line, so a Z overrun is reported with
the lines of those two moves (`z_span_over_mm`, `z_deepest_line`,
`z_highest_line` on `cam_verify`): no touch-off height makes it fit.

### Findings about the cutter

`cam::FeedsFor` (`CamFeeds.cpp`) says what can be said about each tool's feeds
and reach — with the arithmetic and its source beside each sentence — and
what could NOT be checked (`unchecked`), because "checked, fine" and "nobody
looked" are different answers. Feeds stay the user's; these are findings, not
prescriptions.

- **Chip load** = feed / (rpm × flutes), against a band per material class
  (from the material of the bodies being cut) scaled by the cutter's
  diameter, or the tool's own `chipload_min`/`chipload_max` when stated (a
  vendor chart for that cutter beats any rule). Under the band is said, not
  shouted: light machines often run under it and still cut. When the band
  needs a feed the machine's `rapid_feed` cannot reach, that is stated too.
- **Reach**: `cam::ReachCheck` on each tool's deepest operation, as its own
  tool or its finishing tool ([setup.md](setup.md#reach)). The same words
  the edit that made the operation returned, less the operation's name.
- **Chip room** for full-width cuts (the `FullWidth` column: profile, drill).

The findings reach the panel's verdict and the `warnings` of `cam_toolpath`
and `cam_post`. `cam_verify` does not repeat them.

## The simulation

### Timeline

`BuildTimeline(program, rapidFeed, model)` turns the parsed file into a
cursor space: the cursor is `(segment, fraction)` and the slider axis is
**path distance**, not a clock. Cutting time can be defended from each
move's own feed; a rapid traverse rate is a fact the setup usually does not
state, and inventing one would put a fabricated cycle time in front of a
user. Time is a readout (`cutting_minutes`, rapids included only when a rate
is stated; a turning cut clocked the controller's way). The timeline holds the
segments that begin an operation or a tool change, for the skip buttons.

### Stock

`StockSim` holds the block the setup starts from (the fresh stock less what
earlier setups removed, [above](#leftover-another-setup-removes)) and the
cursor. Forward is incremental; **backward re-runs from that starting
block**, because a height map is lossy
and a lowered cell cannot say what it was — a full re-run of a real program is
a frame. The rotary twin keeps snapshots ([rotary.md](rotary.md#the-rotary-picture)).

The height map is meshed by runs, not cells (`BuildStockMesh`): equal-height
cells merge along a row and identical rows merge, a vertical wall joins
differing heights (a milled step is a wall), and a skirt closes the block. The
mesh encloses exactly the volume the map describes, including a cut below the
stock bottom; a milled block meshes to far fewer triangles than cells (gated).
The stock remeshes whole, at most 12 times a second while playing.

`StockRenderer` draws it from host-visible, persistently mapped vertex and
index buffers per frame in flight (the `FieldRenderer` pattern): no device
wait except when a buffer grows. It honours the section clip plane, depth-tests
against the scene and is registered in `RegisterSceneRenderers` like every
scene renderer. It takes the scene's light directions, colours and ambient from
`core/Conventions.h`, the same constants the mesh renderer reads.

### Tool glyph

The tool is a `MeshRenderer` instance — static geometry that moves, built once
per tool number and placed by transform each frame. Its mesh revolves the
`TipHeight` profile (so it shares its shape with the removal math), with the
flute at full length and the stated shank above it. A holder is drawn only
when the tool states one (`overall_length` and `collar_diameter`), where it is:
unstated, nothing says where the nut is, and one drawn on the flutes showed a
cutter buried in the part wherever it reached past its flutes -- a collision
nothing checked. The shank then runs on at its width for a length that reads
as a shank. Normals are per segment, blended only across joints within a crease
angle, so a flat shoulder shades flat and a ball's tip stays round; every
triangle winds the way its normals point (gated for all four shapes and for
the stock mesh). The material is matte: a mirror finish slides reflections
across the glyph as the camera orbits.

### Transport

**Simulate** (`job.simulate`) opens a floating transport along the bottom of
the viewport (`DrawCamTransportWindow`): start, back one, play/pause, forward
one, end, next operation, next tool change (only when the job has one), the
scrub bar, speed, a Path toggle and Close, and a readout of the operation, the
tool, the G-code line and the feed. It refuses in words, at the click, when
nothing is generated. Playback:

- ghosts the model bodies at 0.22 (the part inside the block is most of why
  the picture is worth having), re-applied every frame because instance ids
  change on every regeneration;
- draws the path already cut dim and the path ahead bright (ahead of the
  cursor it is usually buried in uncut stock and appears as the cutter
  arrives);
- marks the worst fixture hit; leaves fixtures visible;
- never regenerates: it reads the cached toolpaths and the posted text, and a
  model edit invalidates it rather than re-cutting under the cursor;
- is a session, not a mode, with one exit (`EndCamPlayback`) that restores
  everything — a GUI test asserts nothing is left behind;
- wakes the frame pacer while playing;
- uses a steady clock, not ImGui's, so an offscreen instance can play.

The overlay colours that land on the workpiece (`kDoneColor`, `kAheadCut`,
`kGhostLine`) are calibrated against the stock's brightness; they are the
ones to re-check whenever stock or part shading changes.

`cam_simulate {setup?, at?: 0..1 | "end", play?, close?, resolution?}`
generates toolpaths if they are not current, leaves the app showing the
simulation (screenshot it, or `close`), frames the job once as it opens, and
returns `distance_mm`, `total_mm`, `cutting_minutes`, `removed_mm3`, the
operation, tool, feed, line and position, and `starts_from` (the earlier
setups whose leftover it started from) — so a headless caller can assert a
simulation did something without owning a renderer.

## Tests

`tests/test_cam_honest.cpp` (headless): a toleranced bore aimed, posted,
judged in its band and failed past it; a K7 class read at its size and a
profile aiming; a part on tabs complete, one cut free said; an opening's
slug excused and flagged; a blind tapped coupon drilled to its point and
threaded to its depth, clean; the holes and threads a job leaves uncut.
`tests/test_cam_verify.cpp` and `test_cam_sim.cpp` (headless):
`SeekTo(End())` equals `Inspect`'s map; scrubbing past and back lands on the
same map; the mesh is the map (bounds, volume, below-stock cuts, the triangle
budget); the distance axis is monotonic and ends at the total; the parser keeps
feed, tool change and operation; a rapid is not counted as time nobody
stated; a zeroed-on-corner job plays its own verdict; the check, the compare
and the designed surface are the one-thread answer on every core, and the
other setups' memo hits and misses as it should. The hidden
`cam verify: the board's planar check, timed` (`[.bench]`, `PLANAR_BENCH_DIR`
names a directory holding `chess.dsgn` and the posted files) times the parts
of a check on a job on disk. The GUI cases
`cam/simulation_plays_and_gives_it_back`, `cam/simulate_says_why_at_the_click`
and `cam/a_rotary_part_turns_as_it_plays` run windowed, because an offscreen
instance has no panels and a renderer drawing into the wrong sub-region looks
right there. `tests/render_pixel_smoke.py` covers the renderers.
