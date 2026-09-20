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
gate.** BUILT — see [As built](#as-built).

**Recommendation, in full:** It keeps the one-engine rule, it reuses `CarveSweep`/`SampleModel`/
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
4. **`ClipToMachine` has no test.** It rewrites every move against the work
   area and it is what would have to interpolate A across a split. 617 lines
   of `CamTravel.cpp` have no test file of their own.

And two structural ones, in the order the code asks for them:

- **`CamVerify.cpp` is two subjects** (1,066 lines): the G-code parser, and
  the height-map carve/compare. The parser is precisely what a 4th axis
  touches, and it is the half that has nothing to do with the stock model.
  Split it out before adding a second stock model behind it.
- **`CamStrategies.cpp` grew 31% in two weeks** (1,068 → 1,399) and is the
  largest file in the subsystem. It holds every generator plus the clip
  epilogue plus the rotary stamp. A rotary strategy lands here; give it a
  seam first.

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

- **D2, the feed model.** HALF done. The verifier's half is closed: a
  rotary move has length now -- the arc the tool tip travels, combined with
  the linear part -- so a rotary program's cycle time is no longer
  understated silently. The GENERATOR's half is the real question and waits
  for a strategy: scaling `F` for constant surface speed with a radius floor
  and a stated clamp, which is FreeCAD's answer and the only complete one in
  the survey.
- **D3, round stock.** The blank is DERIVED from the part (its extent along
  the axis, its furthest point from it, plus the setup's margins — the radial
  one taken as the larger of Y and Z, because a box's margins are all a
  `Stock` can state). A round stock mode is the honest fix and is a format
  change.
- **Nothing generates a rotary toolpath.** This verifies files, which is the
  right order for this subsystem — the check exists before the generator, so
  the generator is trustworthy from its first line — but it means the only
  rotary programs it can be pointed at today are hand-written or from another
  CAM.
- ~~Rapids are not checked against the rotary stock.~~ Done. One walk serves
  both cutting and asking, so the two cannot come to disagree about what the
  tool removes -- a rapid is tested against the stock and never carves it.
- **No picture.** `CamPlayback` draws a height map. A radius map has no
  renderer, so a rotary file verifies but cannot be watched.
