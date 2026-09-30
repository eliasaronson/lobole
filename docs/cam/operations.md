# CAM operations

What an operation is, how the verbs derive one from a click, and how each
strategy turns it into moves. The data is `cam::Operation` in
`src/core/cam/CamJob.h`; derivation is `CamDerive.cpp`; the 2.5D strategies
are `CamStrategies.cpp` (with `CamGeometry.cpp` and `CamAdaptive.cpp`), the
surfacing ones `CamSurfaceStrategies.cpp` over `CamSurface.cpp` and
`CamDropCutter.cpp`, engraving `CamEngrave.cpp` over `CamSketchCurves.cpp`.
The 4th-axis operation is in [rotary.md](rotary.md).

## Operation types

| type | name | what it cuts |
|---|---|---|
| `Profile` | `profile` | follow a contour, the tool outside or inside it by its radius (or `on` it) |
| `Pocket` | `pocket` | clear a closed region, islands respected, concentric rings |
| `Face` | `face` | skim the top of the stock down to the part |
| `Drill` | `drill` | plunge, peck or bore at picked holes |
| `Adaptive` | `adaptive` | clear a region at near-constant engagement (libarea) |
| `EdgeRound` | `edge_round` | break a wall's top edge: a chamfer with a V-bit, a round with a ball |
| `SurfaceParallel` | `surface_parallel` | raster finish of a curved or tilted surface; with a stepdown, a 3D rough |
| `SurfaceWaterline` | `surface_waterline` | constant-Z laps of a steep surface |
| `SurfaceRotary` | `surface_rotary` | 4th-axis finishing and roughing ([rotary.md](rotary.md)) |
| `Engrave` | `engrave` | follow a sketch's curves ON the curve: a groove, down from the surface the sketch lies on ([below](#engrave)) |

Common fields: `toolNumber`, `targets` (`Target{jobPartId, ElementRef}`),
`topZ`/`bottomZ` (setup space; top is where the cut starts, usually the stock
top), `stepdown` (0 = one pass), `stockToLeave`, `enabled`. The per-type
fields are in the capability table below. The operation list's order is the
authored cutting order ([setup.md](setup.md#cutting-order-and-split-output)).

An operation with several targets is generated once per target: five holes
picked into one operation are five bores, in the order given. A target that
cannot be followed goes into the toolpath's `skipped` (and a `SKIPPED` comment
in the file) without costing the others; `error` means the operation produced
nothing at all, and the post skips it with a comment saying why.

## Derivation — what a click means

`cam::DeriveOperation(setup, bodies, type, target, toolNumber)` is the one
place an operation's numbers come from, for the GUI verbs, for `add_cam_op`
without depths, for the panel's Add operation and for `cam_derive_op` (the dry
run). Every depth is derived from the face, the stock and the tool, so a
depth pair like "top 2, bottom −0" that produces nothing cannot be expressed.
The panel's **Add operation** asks `cam::AddOperationType` what the setup
calls for — another `surface_rotary` on a setup that already cuts on the 4th
axis, the whole-part cut-out otherwise — rather than adding one type
everywhere: a 3-axis contour round a turned part means nothing.

| verb | pick | derives |
|---|---|---|
| Clear a pocket | the pocket's floor (`PickedFaceRegion`) | boundary = the face's outer wire, islands = its inner wires; bottom = the face's Z; top = the stock top as the earlier setups left it (below). A pocket deeper than one tool diameter comes back as `adaptive` when libarea is built |
| Cut it out | a wall (`PickedWallContour`) | the contour the wall belongs to; depths = the wall's own extent; inside for an opening, outside for the part's perimeter |
| Cut it out, on a level face with `side: inside` | a face | every opening in that face in one operation; a boss is not an opening; one too narrow for the cutter is `skipped`; `drilled_bores` leaves round openings up to that diameter to the drill |
| Cut out the whole part | none | the part's outline, through the stock, with tabs |
| Drill a hole | inside the hole (a cylindrical face) | top = stock top; bottom = the bore's bottom, or, for a through hole, the bore's bottom minus the point's own height (`TipHeight`) so a cone breaks out; peck = one diameter when deeper than three. Through is where the hole comes out: nothing solid just below its bottom at its centre (a section there), not the part's lowest point, which on a turned part is the grip |
| Break an edge | the wall whose top edge to break | the break the model has there (top = its top edge, `edge_size` = a chamfer's width or a round's height), else on a sharp edge the biggest the tool can make within the wall; a flat mill is refused |
| Face the top | none | the stock above the part, top = stock top, bottom = part top |
| Finish a surface / Finish walls | a face of any orientation | its XY silhouette as the region |
| Rough a surface | none | the whole-part raster finish, then stepdown half the cutter and a skin of `DefaultFinishStock(tool)` |
| Clean up what's left | none | the setup's last surface finish, re-made with the verb's smaller tool and `restFromTool` = the finish's tool |
| Engrave (`job.engrave`; `add_cam_op` type `engrave`) | a sketch entity, `{feature_id: <sketch>, element: "g<entity id>"}` | top = the sketch's plane in the setup; a V-bit's `width_mm` a quarter of the cutter (a text label's: its cap height / 8), a flat's or ball's `depth_mm` a quarter of the cutter; bottom = top minus the depth ([Engrave](#engrave)) |

**The stock top is what the earlier setups left.** `DeriveOperation` takes
the job's setups cut before this one (`EarlierSetups`: the job's order is the
cutting order), and a top derived from the stock — a pocket's, a contour's, a
drill's, a 3-axis surface pass's (`TopFromStock`) — comes from
`StockTopOver`: on an indexed setup after a TURNING one, the top of the turned
part over the operation. That is the model's largest radius along the span the
operation cuts (its targets' faces, widened by the cutter's radius; the whole
part without targets), plus the turning's `stock_to_leave`, its scallop (a
ball's cusp at its stepover, a flat end's facet across the radius) and the
tessellation, taken above the axis where the span comes nearest it. A span the
turning did not reach whole — past its target, or inside the chuck's or the
tailstock's keep-out — keeps the bar's top, and so does a setup whose part
does not sit on the turning's axis. The top is the OPERATION's: `add_cam_op`
with several targets takes it over all of them, and an operation cut at
several indices (`index_a_deg`) reads it in each angle's frame, over the
targets that angle cuts, and keeps the highest — its one `top_z` serves every
angle. `RederiveDepths` does the same. `cam_toolpath`'s
"top_z is below the stock top — entry rapids will pass through stock" warning
reads the same top, so a typed depth at the turned surface is not warned
about.

Derived numbers are fractions of the cutter: **stepdown** = half the tool
diameter (`DerivedStepdown`); **stepover** = 45 % for a flat mill, and for a
ball the step that leaves a cusp of 2 % of the tool radius,
`s = 2·sqrt(2·r·cusp − cusp²)` (`DerivedStepover`); **finish stock** a tenth
of the roughing cutter. Re-pointing an operation at another tool
(`cam::Retool`, `set_cam_op {tool}`, the panel's `T1 T2 …` buttons, the verb
window's tool row) carries a stepdown still at the old tool's derived value
to the new tool's, keeps one the user typed, and refuses a tool the setup does
not have.

What derivation refuses, by name and with what to do instead: a curved or
tilted face for a verb that needs a level one ("pick a level face"), and a
level one that faces down, away from the spindle ("points down"); a wall
where a floor is wanted and the reverse; a hole that does not point at the
spindle ("lay a face up"); a cutter wider than the hole; a V-bit or drill on
anything with a floor (pocket, face, adaptive — the `NeedsFlatFloor` column:
a cone ploughs a groove as deep as the pass, and no stepover fixes that); a
V-bit or drill on a surface finish; an inside profile with nothing to follow.
"Cut it out" on a level face that has material standing round it refuses
rather than routing a full-depth contour round a pocket floor.

## The 2.5D pipeline

```
bodies --SectionCaps--> planar cross-section at Z
       --wire walk-->   ordered 2D loops (Loop{pts, hole, boreDiameter})
       --Clipper2-->    offset / clear
       --link-->        moves with entry, plunge and retract
```

`SectionLoops` sections the (framed) bodies at a Z; Clipper2 offsets in
scaled integers with one mm↔int convention; everything is headless — bodies
in, moves out (`CamToolpath.h`).

### Depth passes and entries

`DepthPasses(op)` visits `topZ − stepdown, …, bottomZ`, and the last pass
lands **exactly** on `bottomZ`: a short final pass is correct, one a hair deep
is scrap.

- **Ramp.** A loop is entered by ramping down along the toolpath at 3°
  (`kRampAngleDeg`) from the height the previous pass already cleared, never
  by plunging: a vertical plunge with a non-centre-cutting end mill rubs. The
  lap then starts where the ramp finished. A loop shorter than the ramp
  descends over one lap.
- **Entry feed** (`EntryFeed`, `CamJob.h`). The plunge feed exists to
  limit how fast the tool goes down, so on a ramp or a helix it holds only
  the vertical component: `min(feed_cut, feed_plunge / sin θ)`. A level or
  rising move gets the cutting feed, and a straight plunge (drilling, a tab
  exit) gets the plunge feed. Each segment is measured from where the tool
  really is: the first one starts at the rapid floor above the ramp and is
  as steep as that makes it. A 3° ramp comes down at `sin 3°` of its feed,
  63 mm/min at 1200, so it runs at the cutting feed. At the plunge feed it
  cut at a third of the chip load. On a small hole, whose ramp fills the
  whole lap, that was nearly all of the hole. This is pycam's rule; FreeCAD
  ramps at the horizontal feed.
- **Down between passes.** A profile with one loop and no lead-out leaves
  the tool down at the end of each lap. The next pass finds it where its
  ramp begins, at the depth it ramps from (`EmitLoop`'s `stillDown`), and
  carries on with no lift and no feed back down the slot. The tool never
  stops against the wall, so this leaves no dwell mark either. A lap that
  ends in a lead-out, or alternates with other loops, still lifts.
- **Helix.** A pocket region too small to ramp around is entered on a helix
  inside it, on the circle the tool centre can walk (inscribed radius minus
  tool radius, bracketed by the insets the pocket already computes).
- A lateral rapid that starts at the bottom of a cut climbs straight up
  first. Drilling plunges, being a plunge by definition.
- The reported cut length is slightly over the true length (a fraction of a
  percent), because the ramp ends mid-segment and the lap restarts there.

### Profile

- **Follows the part's projection, not its section.** A cutter at depth z
  removes the whole column above it, so it may only travel where everything
  between the operation's top and z is waste. The contour at each pass is
  `ProjectedOutlines` — the union of sections from `topZ` down — so a bore
  opening onto a side face does not pull the profile into a notch with solid
  above and below it. A picked wall is re-resolved against the projection per
  pass: each depth takes the loop nearest the pick, unless that loop is more
  than a tool radius away. Then the picked wall is not at that depth at all
  (an opening under a flange recess: above the recess floor the projection
  holds only the recess's wall), and the depth cuts the picked wall's loop
  from the nearest depth that has it, the one below first. Following the
  nearest wall anyway sent the top pass round the recess, in air, and left
  the next pass two stepdowns to take at once. Projecting from the
  operation's top (not the stock's) is what lets an opening that starts
  below the stock top be profiled. This is the expensive step of a profile
  and runs on the background worker.
- **Tabs** (`tabCount`, `tabWidth`, `tabHeight`; derived as four tabs, twice
  the tool diameter wide, a fifth of the depth tall with a 0.5 mm floor, on
  any profile that is not cutting an opening — an opening's slug is scrap)
  split the segments they cross; the ramp and lap are one function of arc length, so a tab is a pillar
  from the bottom of the cut to its stated height and nothing crosses it. Each
  toolpath records its tabs (`Toolpath::tabs`: point, pillar radius, top
  height) so the verifier excuses them, up to their height. `tabWidth` is
  measured along the tool's centre line, where the tool lifts and drops, so
  the material left at the middle of the kerf is `tabWidth` minus the
  cutter's diameter: 8 mm on a 6 mm cutter is a 2 mm tab.
- **Lead arcs** (`leadArcs`): the cut leaves the wall on a tangent arc of the
  tool's radius at the end of every pass instead of stopping against it (the
  dwell mark). Lead-out only: the ramp is the entry, arriving already moving
  along the wall. Refused inside an opening with no room for the arc. The arc
  goes into the waste side (an opening's slug, the stock round an outside
  profile), which nothing has cut, so **every pass leaves the wall at the
  same point**. That point is the middle of the loop's longest edge on the
  first pass, and the nearest point on the wall on each pass after
  (`StartAtPoint`). Each pass starts its ramp one ramp length before it, in the kerf
  the previous pass cut, and the lap ends there (`EmitLoop`). Stacked, each
  arc cuts one stepdown under the one before it. Chained from wherever the
  last lap stopped, each arc landed somewhere new and slotted full width
  from the stock top down to its pass. The arcs of the last passes were
  16 mm of solid MDF at the cutting feed, and moved the board.
- **Corner relief** (`cornerRelief`: `none`, `dogbone`, `tbone`), on profile
  and pocket: at inside corners of the material, computed on the offset ring
  from the turn direction and the side, a bite of the tool's radius along the
  bisector (dogbone) or along the longer wall (T-bone). Off by default — a
  relief is a decision about the joint. Each toolpath records its bites
  (`Toolpath::reliefs`) so the verifier reads them as relieved corners, not
  gouges.
- A through cut's bottom is below the stock, not the part, so the part is not
  left welded to the sheet.

### Pocket and adaptive

- **Islands.** The picked floor's outer wire is where to stop and every inner
  wire an island to go around. `useOutline` (Over openings) crosses the inner
  wires that are HOLES instead of detouring round thin air, but only those
  the cutter cannot enter: an opening the tool fits into stays an island, and
  an inner wire with material standing in it (sectioned just above the floor)
  is kept whatever the option says — no flag may point the cutter at
  material.
- **Open edges.** An edge of the floor with nothing standing just above the
  floor beside it -- a step down, the part's outside, the end of a dome -- is
  not a wall: the region grows by the tool's radius across it
  (`TargetPaths`), so the cutter runs over it, and a wall, which stands above
  the floor, trims the growth back. Every edge taken as a wall, a notch open
  at one end was refused by its length: the chess queen's crown notch, 3.4 mm
  wide with a floor 2.3 mm long before the dome falls away, was "2.3 mm
  across" for a 3 mm cutter. The growth is air, as wide as the cutter asking
  makes it, so a refusal's "N mm across" is measured on the floor as drawn
  (`FloorWidthRefusal`): a slot 0.99 mm wide running out of the part read
  "1.57 mm across" for a 3 mm cutter when the grown region was measured.
- **Inside out, walls last** (`GeneratePocket`). The inset loops of a
  depth are linked into a tree: each loop's parent is the nearest loop one
  level nearer the wall. The tree is cut depth-first, every loop after the
  loops inside it, so the entry and the full-width first cut are in the
  middle of the region, and each wall is the last, stepover-wide cut of its
  side. Cut level by level, an annulus (a flange recess round an opening)
  alternated between its two edges, never cut a neighbour next, and so
  re-entered every ring from safe height, walls first. FreeCAD's default
  order is inside-out; the sequencing by neighbour is Kiri:Moto's and
  blendercam's. The helix enters when the ramp does not fit on the first
  loop, if that loop is the innermost.
- **No loop that clears nothing.** Insetting continues until the region
  vanishes, so where two walls converge the last loop can lie wholly in a
  band its neighbours already sweep. Loops are kept from the walls inward
  with the union of their tool-radius bands, and a loop that adds nothing
  to it (nothing wider than 0.002 mm, which is arithmetic) is dropped. A wall
  never is. Anything coarser leaves a hair of material standing: at 0.1 mm
  it dropped a ring whose neighbours stopped 0.05 mm apart.
- **Concentric rings link at depth.** A ring whose nearest point lies within
  one and a half step-overs of the tool keeps the tool down, and the ring is
  started at that point (`StartAtPoint`). Further than that it lifts. A lap
  can end without retracting for this.
- **Adaptive** (`OpType::Adaptive`, Tusek's `Adaptive2d` from FreeCAD's
  libarea, vendored under `deps/` and linked as the LGPL shared library
  `designer_adaptive`) clears at near-constant engagement, so the full depth
  can be taken in one pass. Parameters map straight across: stepover factor,
  stock to leave, the chord tolerance, helix ramp diameters from the tool.
  Its three motion types map to cut, link-at-depth through cleared material,
  and retract-rapid-re-enter. A link through cleared material is a G1 (a
  straight line, which a dog-legged G0 is not) at the machine's traverse
  rate (`Setup::rapidFeed`) when it states one, else the cutting feed. Its
  failure flags are put in words. When the library flags ANY of them on any
  depth, concentric clearing cuts the whole operation instead and
  `strategy` says why — never an error beside moves, because a program the
  op posts has to be what it reports. It flags trouble on output it still
  fills (a lead it could not plan: a 3.2 mm slot and a 3 mm cutter), and
  keeping both posted a program for an op that said it failed. Finding
  nowhere to start emits nothing: its start point is unset, and entering
  there anyway made moves that kept the fall-back from running.
- Both refuse cones (`NeedsFlatFloor`).

### Face

Clears the stock above the part, starting with the cutter centred on the
stock edge and spiralling inward at the stepover, down to the part top. The allowance is the stock's Z margin (default 1 mm: a rough board).

### Drill

A hole the cutter's size is plunged, fed from the top of the cut (a rapid to
there first, not a feed from safe Z). With `peck`, the tool retracts to the
hole's top between pecks (the G83 reading, as plain moves — no canned
cycles) and rapids back to just above the last floor. A hole **wider** than
the tool is **bored**: a helix down at the ramp angle on the circle the tool
centre walks, a flat turn at the bottom, and for a hole wider than two tools
rings from the centre outward by the stepover, the wall last so a through
hole's slug goes progressively. A hole narrower than the tool is refused. A
hole must point at the spindle.

A bore's rings each begin with a helix, and each climbs straight out of the
hole before it travels (`EmitHelix`): a rapid that climbed and travelled at
once went through the wall of a hole not yet cut to size. A pointed tool in a
blind hole whose floor the model has flat is said at derivation
(`DerivedOp::notes`, in the RPC's `warnings`): the point leaves a cone of its
own height, which the verifier would otherwise first report as leftover.

### Edge break

Follows a wall's top edge and breaks it with the tool's own shape: a V-bit
chamfers (tip angle is the included angle; `edge_size` is the bevel's
horizontal width), a ball rounds (`edge_size` is the radius). `stepover` sets
the cusp between arc passes. A flat mill is refused rather than cutting a
square step nobody asked for.

**Derived from the model's break when it has one** (`FindModelledBreak`).
- **The probe:** sections just above the wall's own top. On a sharp edge
  there is nothing within a tool radius of the wall there. On a chamfered
  or rounded one the material steps back from it up to the top face.
  Bisection finds that face, which is the real edge and the op's `top_z`.
- **The size:** the break's width there for a chamfer (exact at its top),
  or its height for a round (a round meets the face tangentially, so its
  width just below falls short).
- **Refused:** a V-bit whose angle is not the chamfer's, naming both, and a
  ball on a break that is not a round.
- **Before this:** the edge was the wall's top, below the modelled break,
  and the size the biggest the tool allowed. A 1.5 mm chamfer was cut 6 mm,
  and the verdict's own advice ("model the break") could not help.
- **A sharp edge:** the biggest break the tool can make within the wall,
  as before.

### Engrave

`OpType::Engrave` follows the curves of a SKETCH entity ON the curve, not
offset from it: a line, an arc, a circle, an ellipse, a spline, open or
closed, and a text label -- in the single-stroke font (`hershey`) every
pen-down stroke of every glyph, in the outline font every glyph's outline
([text](../sketching/README.md#text)). It is the one
operation whose target is not a face of a body, so its target is
`{feature_id: <the sketch>, element: "g<entity id>"}`, the entity's stable id
(the same `g<id>` a sketch-made face is named by). Several targets are one
operation, every stroke of every one ordered together.

- **Resolving the curves.** A sketch makes no body, so the generator cannot
  find its curves among the bodies it is handed. The caller that has the
  documents resolves each target (`cam::SketchEntityCurves(doc, ref)`, in
  `CamSketchCurves.cpp`: the entity's strokes in the part's model space, its
  sketch plane, and a text label's cap height) and hands them in
  `GenerateContext::sketchCurves` and to `DeriveOperation`
  (`ui::JobSketchCurves` in the app; a test resolves its own document). A new
  sketch entity type is taught to be engraved in ONE place, `StrokesOf` in
  that file, and the build fails there until it is. Refused, by name: a
  feature that is not a sketch, an element that is not `g<id>`, an entity the
  sketch does not have, construction geometry and a point. An engraving
  handed no curves for a target refuses rather than cutting nothing.
- **Where it cuts.** The curves go through the sketch plane, the job part's
  placement and the setup's frame (`BodyToSetup`, the one transform bodies
  take). The plane must face the spindle; one that does not is refused,
  naming its angle (lay the face up, or turn it up with `index_a_deg`: at
  several indices each target is cut only where its plane faces up, as a
  floor is). The top is the sketch plane in the setup, and it is probed along
  the strokes (`EngraveSurfaceRefusal`): material just under the plane and
  none just over it, or the operation is refused, naming where the curve
  leaves the surface, rather than cut in the air or sunk into the part. The
  top is read from the plane at every generate; `top_z` and `bottom_z` are
  what the derivation reported.
- **Depth from width.** `width_mm` is the groove's width at the surface and
  `depth_mm` its depth, and they are one statement, not two: setting one over
  the RPC or in the panel clears the other. For a V-bit
  `depth = TipHeight(tool, width / 2)`, the verifier's own function, so the
  two cannot disagree; a width past the cone (wider than the cutter) or a
  depth past its height is refused, and so are a width and a depth that
  describe different grooves. A flat or a ball takes `depth_mm` only (a
  flat's groove is its own width); a drill is refused. Derived: a V-bit's
  width is a quarter of the cutter, or a text label's cap height / 8, never
  wider than the cone; a flat's or a ball's depth is a quarter of the cutter.
  With stock standing above the surface the derivation says so in its notes,
  and `cam_toolpath` warns unless a face operation before it takes that off
  (face it first).
- **Order.** Label by label (a label is one target): the authored first, then
  whichever is nearest where the last finished. Inside a label, nearest
  neighbour over its strokes -- an open stroke reversed when its far end is
  nearer, a closed one started at its nearest point -- then 2-opt, which
  reverses stretches of the sequence where that shortens the joins
  (`OrderStrokes`). A stroke that starts where the last one ended keeps the
  tool down.
- **Moves.** Onto a label from safe Z: a rapid down to the stock top, then a
  straight plunge at the plunge feed (a V-bit's point cuts at its centre, and
  a 3-degree ramp is longer than most strokes). The stroke at the cutting
  feed, all at one depth. Between strokes of a label a short hop, a
  clearance over the surface (a millimetre, or the tool's radius if less);
  one that would pass through the part at that height goes over at safe Z.
  Between labels, safe Z.
- **Told to the verifier.** Each stroke is a `Toolpath::Groove` (its
  polyline, tip height, surface and tool): see
  [simulation.md](simulation.md#engraved-grooves).

The capability row is `width_mm`, `depth_mm` and `index_a_deg`; every other
field is refused by name. In the GUI the job tab draws the parts' labels and
open curves, and **Engrave** (`job.engrave`) is a click on one: the operation
derived with the setup's V-bit, a further click adding a label to it, and a
setup with no V-bit refused where the verb was pressed, naming the tools it
has ([gui.md](gui.md#engrave-from-the-viewport)). On a setup in the rotary a
label on a side is turned up by the index its plane solves
([gui.md](gui.md#indices-from-picks)). Its width and depth are edited in the
op's settings.

## Rough then finish

`finishTool` / `finishStock` on profile, pocket, face, drill, adaptive and
edge round: the operation roughs with its own tool leaving `finishStock`
(default a tenth of the roughing cutter) on the wall and in Z, then a second
toolpath with the finishing tool takes the final boundary in one full-depth
pass. One operation with two tools rather than two operations that must agree
about a boundary; tool ordering groups the two toolpaths. The panel's
operation settings offer "finish with T<n>".

## 3-axis surfacing

A finishing pass is a **pattern** swept over a **contact surface**, and the
two are independent.

- **The cutter-location surface** (`CamDropCutter`): for each (x, y), the
  lowest Z the tool can sit at without cutting the model — the designed
  surface sampled on a grid (`SampleModel`) dilated by the tool's own profile,
  `z_CL(x,y) = max over p within the radius of (z(p) − TipHeight(tool, d))`.
  `TipHeight` is the same function the verifier carves with. A sample
  stands for the square of material round it, so `d` is the distance to
  that square's nearest point, not its centre. Measured at centres, the
  model ended half a cell short of a sharp convex edge, and near its rim a
  ball's tip is steep enough to turn that into most of a millimetre of
  gouge. The nearest point errs by at most half a cell toward leaving
  material, never toward the part. The drop cutter takes a frame and refuses
  a rotated one by name rather than dropping along the wrong axis.
- **The region** (`PickedSurfaceRegion`): a face of any orientation,
  tessellated and projected to its XY silhouette; with no pick the whole
  part's silhouette. The face contributes only its outline — Z always comes
  from the CL surface of the whole model, or a pass would gouge the material
  beside the face.
- **`surface_parallel`**: raster passes at `cut_angle_deg` (anticlockwise
  from +X), spaced `stepover` (a fraction of the cutter) across and sampled
  `sample_step_mm` along. Passes are ordered nearest end to nearest end and
  neighbouring passes are linked by a cut straight across rather than a lift,
  which also keeps a ball's rim out of the uncut strip beside a steep groove.
  A pass ends at any sample with an unreached cell among its four neighbours,
  and takes the highest of four where they differ steeply, so bilinear
  interpolation cannot undercut a cliff or a masked cell.
- **`surface_waterline`**: constant-Z laps (marching squares on the CL
  surface), `z_step_mm` apart, and closer where the surface flattens.
  - Evenly spaced levels spread apart on a shallow slope, and the cap above
    the highest lap was never cut (0.6 mm left on a dome's top).
  - Between two levels, the band between their regions is measured on the
    CL grid, inside the region: area over perimeter, the perimeter scaled
    by pi/4 for its staircase. A band wider than the spacing that leaves
    the finish's own cusp (`2 sqrt(2rc - c^2)` for a ball, half a flat
    cutter) gets a level at its middle height, recursively.
  - The cap, from the surface's own top, is cut from its rim only, so it
    is held to a quarter of that.

Three quantities, three names: `stepover` (across passes, a fraction of the
cutter), `sample_step_mm` (along a pass, mm; 0 derives from the CL grid) and
`z_step_mm` (between laps, mm; 0 derives from the tool). None is the others.

A surface op refuses every field that presupposes a contour and discrete
levels — `side`, tabs, `corner_relief`, `lead_arcs`, `drilled_bores`,
`use_outline`, `peck`, a finishing tool and `climb` — by name. Waterline also
refuses `stepdown`; a raster's stepdown means roughing (below). V-bits and
drills are refused.

Arc fitting leaves a sloped raster alone (an arc must be a cut at constant Z
with no A word) and fits a constant-Z waterline lap.

### Rough a surface, and rest machining

- **3D rough**: a `surface_parallel` with a `stepdown` cuts in layers from
  the stock top down to the surface plus `stock_to_leave`. A cell with nothing
  left above its floor is unreached for that layer, so the pass breaks there
  instead of dragging the tool through cut air. The region is the part's
  outline plus a ring one cutter wide, cut down to the op's bottom, so the
  finishing ball can reach the part's edges while the rest of the block stays
  to hold the part. A finish derives no stepdown, which is what keeps a
  finish a finish; a raster finish saved by a build that derived one loses it
  on load, so it is not read as a rough. Waterline roughing is not built.
- **Rest machining** (`restFromTool`, on `surface_parallel` and
  `surface_rotary`): the number of the larger cutter that finished before.
  Both cutters' CL surfaces are computed and each tool's swept envelope taken
  from its own; the pass cuts only where the larger tool leaves measurably
  more than both the smaller tool and the part itself (a point probe of the
  part), so no rest passes run round the rim over air. Envelopes are taken
  over blocks a third of the small ball's radius across, conservative in the
  direction that only adds cutting. A rest pass after a waterline finish is a
  raster: the region, not the pattern, is the point. The verb refuses without
  a finish to follow or with a tool that is not smaller.

### Surface entries

A pass that is not linked to the one before rapids to safe height over its
start, rapids **down to just above what is known to be left there**, and
feeds only the rest at the plunge feed. The same rule as a 2.5D pass's
`RapidFloor`, with more known. Each case is planar and rotary alike; on the
rotary, the descent is radial:
- **Nothing known** (a finish, the first layer): the stock top (the blank's
  radius on the rotary).
- **A later layer of a rough**: the floor the layer before left, plus the
  ridge between its passes (a ball's scallop at the stepover).
- **A rest pass whose finish is cut first** (`FinishedBefore`, in the
  setup's cutting order): the highest leftover the bigger cutter's envelope
  leaves under this tool's footprint, plus the finish's skin and cusp. Its
  envelope is taken per block from the block's highest tip, so it can only
  overstate. Where the finish left nothing, the pass comes down to its own
  surface. A rest op cut before its finish knows nothing, and enters from
  the stock top.
- On the rotary, a cell no pass of the op reaches still stands at the blank,
  and an entry beside one comes down no lower than that.

The entry stops a clearance above what is known to be left: a millimetre, or
the tool's radius if that is less. The verifier's
`rapids_through_stock` is the check on all of it. The rotary sampler's rest
pass had spent 26 of its 58 minutes feeding down from the safe radius through
air its finish cut.

## The capability table and the field table

`cam::Operation` is one struct for every type; two tables keep that honest.

- **Capability** (`CamNames.cpp`, one `OpTypeRow` per type, in enum order,
  `static_assert`ed against `OpType::Count`): the type's name and an `OpField`
  mask of the fields its generator reads, plus per-type facts `FullWidth`,
  `ClearsInside` and `NeedsFlatFloor`. The panel's field guards, the RPC's
  answer and its refusals, the feed checks and the edge-break census all read
  it. A field the type does not have is refused at the RPC by name rather than
  stored where nothing reads it. `test_cam_capability.cpp` enforces the table:
  for each type, every field it does NOT declare is poked (in bulk, bisected
  on failure) and the moves must be identical; a negative control pokes a
  declared field and requires the path to move. Over-claiming (a field shown
  that does nothing) is allowed; under-claiming (a field that silently steers
  a cut) fails.
- **Fields** (`CamOpFields.cpp`, one `OpFieldRow` per field: file/RPC key,
  capability bit, pointer to member, flags): `WriteOpFields`/`ReadOpFields`
  serialize every field that differs from a default-constructed `Operation`;
  the RPC's owned list, apply step and answer loop over `OpFieldRows()`; the
  surfacing cache key hashes it; `tools/rpc_params_scan.py` reads it to check
  the `describe` rows. Enums read and write through the name tables and
  refuse an unknown name with one wording. `tabs` and `finish_tool`/
  `finish_stock` are saved, applied and reported by hand (flagged
  `kOpFieldByHand`), and so is `side`'s type-dependent load default: a
  clearing operation saved with no `side` key loads as `inside`
  (`SerializeCam.cpp`). Adding a field is the member, a table row, the panel
  widget and a poke in the capability test.

The round trip is gated both ways: `Operation` has a defaulted `operator==`,
and every field of every type goes through save and load and must come back
equal; `headless_smoke.py` sets each field an op reports through `set_cam_op`
and requires `list_cam_setups` to report it back.

A new `OpType` without a row fails the `static_assert`; without a case in a
switch it fails the build (`-Werror=switch` on `src/core/cam/`); the name
functions never fall back to `"profile"`.
