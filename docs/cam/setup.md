# CAM setups: machine, stock, parts and fixtures

Everything a setup says before an operation exists: which machine, how the
parts lie, what the workpiece is, where zero is, how high the tool flies, and
what holds the stock down. The data is `cam::Setup` and its members in
`src/core/cam/CamJob.h`; the arithmetic is in `CamSetup.cpp` and
`CamTravel.cpp`.

## Machine profiles

`cam::Machine` is one physical thing: `name`, the work area (`travelMin`,
`travelMax`, zero-size = not stated), `post` (`mach3` | `linuxcnc` | `grbl`),
`fitArcs`, `hasRotaryAxis`, `rapidFeed` (the traverse rate, mm/min; seeds
the setup's, caps X/Y/Z on a turning cut, and is the ceiling the feed check
uses; 0 = not stated),
`rotaryMaxFeed` (the A axis's top speed in deg/min; saved as `a_max_deg_min`;
0 = not stated), `rotary` (how the rotary is installed: the axis it lies
along, the chuck's end, its letter and which way it turns;
[rotary.md](rotary.md#how-the-rotary-is-installed)), `holding` (the chuck's
and tailstock's sizes; [rotary.md](rotary.md#chuck-and-tailstock)) and
`library` (tools). The settings window's machine editor has all of them.

**Stored globally, one file per machine**: `~/.config/designer/machines/<slug>.json`,
beside `settings.json` — a list of structs does not belong in the scalar
settings table. `DocumentIO::LoadMachines/SaveMachine/DeleteMachine` own the
store; `ui::ReloadMachines` mirrors it into `state.machines` at startup (the
GUI, `--offscreen` and `--headless` all load it) and after every write.
`headless-preview.sh` copies the store into its scratch config so a preview
sees the shop's machines without being able to edit them.

**Referenced by name and embedded.** A setup carries `machine`, a full copy of
the profile it was set up on; the name is the working reference. On load and
in the panel, `cam::MatchMachine` compares the copy with the store:

| state | meaning | what the panel offers |
|---|---|---|
| `Unnamed` | the job's own machine | — |
| `Global` | named, equal to yours | — |
| `Differs` | named, yours says something else | the job's copy is used; *Use mine* / *Save as mine* |
| `Missing` | named, not one of yours | the job's copy is used; *Save it* |

The choice is never made silently: the differing field is the one that
matters (a shorter bed, a post that writes another arc word). The writer
always embeds the whole profile, so a file cannot name a machine it does not
carry. `list_cam_setups` reports `machine` and `machine_match`.

**Tools come from the machine and are copied, not linked.** `ui::ApplyMachine`
is the one place a setup's machine is assigned; it seeds `rapidFeed`, zeroes
the rotary index on a machine without the axis, and, on a
setup with nothing cut yet, copies the library into `tools` — an operation
names its tool by number, so replacing the table under existing operations
would re-point every cut. The Tools header's `+` offers the library first.

## Setups

A setup is one clamping of the job's parts: one stock, one touch-off, one
machine, one list of fixtures, tools and operations. The panel adds (`+`),
renames, switches, deletes (`x`) and **flips** setups: Flip makes a new setup
whose frame is the active one turned half a turn about the setup's own X
(`cam::FlipFrame`), which is the second side of a flip job derived rather than
typed. Each setup posts its own file. The job's list of setups is the order
they are cut in (`EarlierSetups`): a setup's check, its playback and the tops
its operations derive start from what the setups before it left
([simulation.md](simulation.md#leftover-another-setup-removes)).

A setup arrives with one 6 mm tool (a setup with no tools can produce
nothing) and a derived safe Z.

## Reach

Whether a tool reaches an operation's depth (`topZ − bottomZ`) is a fact
about the operation and the tool alone, so it is asked when the operation is
**made**, not first when the file is posted. `cam::ReachCheck(op, tool)`
(`CamFeeds.cpp`) is called by every edit that can change the answer:
`add_cam_op`, `cam_derive_op` and `set_cam_op` for that op, and a tool edit
in `set_cam_setup` for every op that cuts with the tool, its own or as its
`finish_tool` (`ReachChecks`). The panel's operation and tool editors call
the same function.

A thread mill reaches with its neck as well as its head: its reach is
`flute_length` (the head's width) plus `neck_length` (`ReachLength`), and the
shank probe starts above both ([the thread-mill
cutter](operations.md#the-thread-mill-cutter)).

Above its flutes a tool is `shank_diameter` wide (v131). Left at 0, the tool
is taken to be the cutter's width all the way up, as every check assumed
before the field existed. Graded by what happens:

| | past the flutes | edit |
|---|---|---|
| shank **wider** than the cutter (a 3 mm bit on a 6 mm shank, a V-bit's body) | driven into the wall | **refused**, naming the op, the depth, the flutes and the shank |
| shank the cutter's width, or unstated | rubs, and the holder follows it into the work | allowed; said in the edit's `warnings` |
| shank **narrower** (a necked tool) | reaches, by design | allowed; the holder check owns the rest |
| within reach, less to spare than the collet nut needs (`CollarClearance`, below) | touches | allowed; said |

Where the nut is: the tool's stickout (`overall_length`) when stated; else
the top of the flutes; and on a tool narrower above its flutes than in them
(a V-bit's cone, a necked cutter), nowhere this can say, so there is no
"Tight" for it. The clearance scales with the shank the collet grips when
it is stated, else the cutter. A 12 mm V-bit's 1.5 mm chamfer was once
"the depth where the nut touches the work".

A refused edit changes nothing: the RPCs apply it to a copy, and the panel
takes the field back and shows why. A disabled operation cuts nothing and is
asked again when it is enabled. `surface_rotary` is not asked, since its Z
span is the blank's diameter, not a depth. A document saved before this can
still hold a refused op, and `FeedsFor` states it for the post. Where the
depth cannot tell, for example an op whose `top_z` sits under a pocket
narrower than the shank, the verifier finds the shank at the wall
([simulation.md](simulation.md#shank-collisions)).

## Parts and placements

`JobPart{id, sourceDocId, name, configuration}` references a part document,
in one of its configurations (v135; 0 = its active one -- `add_job_part` /
`machine_part {configuration}`); the job owns no geometry. `ui::JobBodies`
returns every part's bodies in model space (`ui::JobPartDocument`: the part
in its configuration), each stamped with its transient `jobPartId`; its
cache key is the job's version plus every source part's version,
configurations revision and the configuration it cuts, so a model edit
reaches the toolpath the frame it happens. A source that is gone, or no
longer builds in that configuration, is reported by `list_job_parts`
(`missing`, `why`), never substituted. `ui::JobPlacedBodies` is the same list laid out in the
active setup's frame — what the job tab draws and what is cut.

A **placement** (`cam::Placement`) says where a part sits in this setup's
stock: `pos` (setup XY, mm), `rotZdeg` (spin under the spindle), `enabled`
(on the sheet, not cut this run) and an optional `frame`. Position and spin
are in-plane on purpose: anything out of plane is what the fixturing frame
says, and two ways to tilt a part is one too many.

Placements are checked when asked, not while dragging (`cam::CheckPlacements`,
`check_layout`), because a sheet is laid out *through* bad states:

- `fit`: `inside`, `partly_outside` (hanging over an edge) or `fully_outside`
  (not on the sheet at all) — different mistakes, reported differently. Always
  `inside` for relative stock, which is drawn around the parts;
- `overlaps`: the placements it runs into;
- `box`: its extent in setup space;
- per setup, `warnings` when two parts sit closer than the widest cutter — a
  warning, because only the tools that go between them care.

`cam::PlacementRefusal` stops generation (every operation's `error`) and the
post when a part is placed twice (repeats are not cut yet), off the sheet,
partly off it, or on top of another part.

## Fixturing frame and rotary index

Two different physical claims, two fields:

| field | question | posts |
|---|---|---|
| `Setup::frame` (and `Placement::frame`) | how is the part clamped on the bed? a fixturing fact | nothing |
| `Setup::rotationA` | what angle does the rotary table move to? a machine fact | `G0 A…` |

A part is modelled where it belongs in its assembly — a speaker baffle stands
upright — and clamping it flat is a statement about the job, not the model.
`frame` is the rotation taking setup space to model space; `SetupToModel`
composes `frame · R_x(-A)` (the index turns the part, so it is undone going
back to the model), and a test pins that a 90° frame and `rotationA = -90`
cut the same toolpath, differing only in the A word.

`rotationA` needs `machine.hasRotaryAxis`: an index on a machine without the
axis is refused, and `EmitsRotary` (the axis exists AND the index is off zero)
is the question the toolpath asks before stamping the setup's index on its
moves. The other source of an A word is an operation's own index list
(`index_a_deg`, which also needs the axis): its moves carry the angle each
piece is cut at, A0 included
([rotary.md](rotary.md#one-operation-at-several-indices)). The post writes an
A word only for a move that carries one. A 3-axis job with a rotated frame
posts no A word.

**Lay a face up** (`SolveFaceUp`) returns the frame that puts a planar face's
normal on +Z. Any planar face has one. A normal fixes two of three rotations;
the spin about the spindle is taken as the shortest arc from the frame in
hand, so a click changes as little as possible and repeating it is idempotent.
**Turn a face up** (`SolveIndexA`) is the rotary path: the A that brings a
face to +Z by rotating about X, refused for a face pointing along X. The face's
normal is read in the setup's frame with no index (the fixture frame and the
placement applied); the index turns the part by R_x(A), as the mount, the
rotary check and playback all do, so a +Y face comes up at A = 90.

**Per-part fixturing.** `Placement::frame` is optional: set means "this part
is clamped this way", unset means "as the setup says" (`cam::FixtureFrame` is
the one place that is answered). Optional rather than defaulted to identity,
because a default would un-clamp a part the moment it was given a position.
So one sheet can hold a baffle on its face beside a rail on its edge.

One rule for both doors, the GUI pick and `set_job_placement {lay_face_up}`:
a part that HAS a placement is clamped there; in a one-part job with no
placement the setup's frame is the answer; in a job of several parts, laying
one part's face up gives that part a placement rather than re-orienting the
others. `set_cam_setup {lay_face_up}` sets the setup's own frame; in a job
of several parts it takes `lay_face_up.part`, because every Derived part's
feature is id 1 and a feature id alone names a face on each of them — named
on more than one, the call is refused rather than taking the first.

Re-clamping re-derives every targeted operation's depths
(`cam::RederiveDepths`), since `topZ`/`bottomZ` are heights in setup space; the
count is stated in the panel's note. A Z zero whose face is no longer level is
dropped and the note says so.

## Stock

`cam::Stock`:

- **`Relative`** (the default): the parts' tight bounding box plus `margin`
  (X, Y, and Z above) and `bottomMargin` below. Defaults 2/2/1 mm and 1 mm.
  Margin Z is the allowance a facing pass skims.
- **`Fixed`**: the sheet you own, `size` in mm, and `place` (0..1 per axis)
  saying where the part sits in it — 0 low face to low face, 1 high to high,
  Z default 1 because sheet goods are referenced from the top. Once anything
  is **placed**, the sheet is a real object: it occupies `[0, size]` in setup
  X and Y and the parts are arranged on it; Z keeps the `place` derivation. A
  sheet smaller than the part is refused naming the axis and the shortfall
  (`StockRefusal`), with `kStockFit` (1 µm) as the allowance for OCC's
  bounding-box gap.

`StockBounds`/`StockBoundsFramed` is the one function that answers "what is
the workpiece"; safe Z, derived depths, the origin, the verifier, the
simulator and the plan view all go through it. That box is the FRESH stock:
the verifier and the simulator lower it by what earlier setups removed
(`StartingStock`), and a derived top reads the turned part over the
operation (`StockTopOver`). `list_cam_setups` reports both the stock request
(mode, size, place, margins) and the box it resolves to.

## Work origin

Every coordinate the post writes is relative to `Setup::origin`, the corner
the operator touches off on. It is **derived, not typed**: `originAt` is the
rule that is saved (`stock_min` default, `stock_centre`, `model_min`,
`stock_max`), and `RecomputeOrigin` computes the point in setup space — X and
Y from the stock or model box, Z from the picked top face (`topFace`) or the
stock top when none was picked. It is re-run after anything that moves the
box: the stock, the frame, the pick, the model. The panel offers Stock corner,
Stock centre and Part corner and prints the resulting triple beside the
choice; `stock_max` (the far corner, the one over the table when a sheet is
slid through the bed) is RPC-only. An unknown name is refused.

`topFace` lives on the setup, so a job reopened from its file still knows
where Z zero came from, and two setups keep separate answers. **Pick top
face** also lays that face up (the pick is the statement that the part is
clamped with it up); *Lay a face up* re-clamps without re-answering Z zero,
which is what a flip needs.

How the origin reaches the file is in [post.md](post.md#work-origin-in-the-file).

## Safe Z

`Setup::safeZ` is an absolute Z in setup space. `cam::DeriveSafeZ` puts it
clear above the stock top and, over a setup's fixtures, above the tallest
fixture; `ui::DeriveJobSafeZ` is the one caller on a job tab, so a clamp
taller than the stock cannot be forgotten. On a setup held in the rotary it
clears the radius the stock SWEEPS instead of its top: a square bar's
corners stand at half its diagonal and pass under the cutter every turn (a
pass the holding window clipped at the tailstock retracted to the top, and
the next turn put the chess blank's corners 2.4 mm into the cutter). It is
re-derived when the geometry it depends on moves: a new setup, a flip,
laying a face up, a Z-zero pick, adding, moving or removing a fixture; and
`set_cam_setup` raises it -- never lowers it, so a typed height stays --
when it changes the stock, puts the part in the rotary or changes the
machine.

## The machine's work area

When the machine states a work area:

- **Where the machine sits.** `cam::MachineBounds` places a travel-sized box,
  its floor at the stock's bottom, per axis:
  - **Stock that fits the travel:** centred on it, so a cutter running
    outside the stock (an outline) has room on every side. Anchored at the
    stock's near corner, an outline round a part that fit the machine lost
    its two near sides.
  - **Stock longer than the travel:** its near-left corner at the stock's.
  - `Setup::reachOffset` moves it from there. It is where the bed is on a
    sheet longer than the machine.
  - `set_cam_setup {reach_offset: "auto"}` runs `DeriveReachOffset`. That
    puts the window on the cutting along an axis that does not fit, and
    leaves a fitting axis centred.
- **`MachineWarning`** compares what the spindle has to REACH — the placed
  parts' extent per axis — with the travel. A part longer than the bed is a
  warning, not a refusal: it is cut as far as the machine reaches and the rest
  is another setup with the sheet moved along. **`MachineOverhang`** notes a
  sheet longer than the bed whose parts fit. Clipped cutting is a line of
  its own whenever there is any, naming the operations, parts that fit or
  not. It used to ride on the "bigger than the machine" line alone, so it
  was otherwise a number on the op, and the verifier, reading the clipped
  file, called the uncut strip leftover. `cam::MachineLines` gives the
  panel, `cam_toolpath`, `cam_post` and `cam_verify` the same lines. The bed
  is never grown to fit.
- **`ClipToMachine`** clips every move to the box in X and Y, applied once in
  `GenerateAll`, so the backplot, the simulation, the verifier and the file
  show the same shortened cut. A cut that runs out is cut to the boundary and
  retracts; one that comes back re-enters at safe height and plunges. The
  amount left out is `Toolpath::clippedMm` (`clipped_mm`). A controller does
  not truncate for you — with soft limits it alarms on the line, without them
  it drives into the stop — so the file has to stop.
- **A rotary along machine Y** swaps the X and Y travel for every one of
  these, since the part's axis is setup X
  ([rotary.md](rotary.md#how-the-rotary-is-installed)).
- **Z is a span, not a position.** Where the machine's Z zero sits against
  the workpiece is set at touch-off, after the file is written, so the
  verifier's travel check in Z asks only whether the program's deepest point
  and highest retract are further apart than the Z travel.

## Workholding (fixtures)

A **fixture** (`cam::Fixture`) is what holds the stock: a part document (a
vise, a shaped clamp, a fixture plate, modelled or imported) or a keep-out
**box** (`sourceDocId` 0, a size, no part). It belongs to a setup — you
re-clamp between setups — and lives in **bed space**, not model space: the
setup's frame turns the part to lie on the bed and a vise does not turn with
it. So a fixture's model is read as bed coordinates, Z up, and placed by
`FixtureTransform`: `bed = T(pos) · Rz(rotZdeg) · model`. It never goes
through `BodiesInSetupFrame`.

A fixture is not a job part: it gets no stock, targets or toolpaths, never
grows the stock box and is never a surface a gouge is measured against. A part
the job cuts is refused as a fixture. `enabled` false draws it and does not
check it.

**Placement is derived** (`PlaceFixture`, `FixtureSide`): a side (`-x`, `+x`,
`-y`, `+y`) stands it on the bed against that face of the stock, centred along
it; `centre` centres its footprint on the stock with its bottom at the stock's
bottom (a vise goes round the stock); `top` lays its box on the stock's top
straddling the −X edge. A new fixture is centred when wider than the stock in
X or Y, otherwise it takes the next free side (−X, +X, −Y, +Y). A keep-out
box's default size is derived from the stock (`DefaultKeepOutBox`).

What the verifier does with fixtures is in
[simulation.md](simulation.md#fixture-collisions).

### Seats

A fixture can **rest on the stock** instead of standing at a bed position:
`Fixture::seat` (`cam::FixtureSeat{face, edge, alongMm, overMm, spinDeg}`)
says which of its faces rests on the stock's top, which edge of the top it
reaches in over, how far along that edge (from its low end to the face's
centre), how far in past it, and the spin from pointing straight in. A
face-to-face relation, stored and re-solved: a clamp placed on a board stays
on the board when the stock grows.

- `SolveFixtureSeat` turns a seat into a pose: the face must be planar and
  face −Z (a fixture only turns about Z, so another face is refused, not
  tilted); the nose direction is from the fixture's box centre to the face's
  centroid, turned to point into the stock.
- `SeatFromPose` is the inverse, used when a fixture is dragged onto the
  stock; `FaceOnStockTop` finds the named, flat, downward face lying on the
  top (largest overlap wins).
- Every reader re-solves: `ui::FixtureBedTransform` gives the seated pose from
  the stock as it is now and falls back to the stored `pos`/`rotZdeg` (the
  last solved pose, kept for a reader that cannot solve) only with a reason.
  `JobRenderVersion` includes the stock's definition so the ghost follows it.

Edges are `-x|+x|-y|+y`; an unknown one is refused. Over the RPC,
`set_job_fixture {seat: {feature_id, element, edge, along_mm, over_mm,
spin_deg?}}` seats a fixture and `seat: null` stands it back on its own at its
current pose; a seat together with `pos`, `side` or `rot_z` is refused. Only
the stock's top takes a seat; a vise jaw against a side face is not modelled.

## Cutting order and split output

The operations vector IS the authored cutting sequence (`move_cam_op`, or drag
the row's grip). `Setup::order` (`OrderOutputBy`) says how the post groups
them, applied in `GenerateAll` via `CuttingOrder` (stable within a group):

- `authored` (default): as written;
- `tool`: group by tool, one tool change per tool — what a multi-part sheet
  authored part by part needs. The groups go in the order each tool is first
  used, not by number: a table is not ordered by how early each cutter is
  wanted. And a tool a rest pass uses goes after the tool it rests from,
  because a rest pass cleans up after a finish; run first (a 3 mm ball
  numbered below the 6 mm finish it follows), it did the finish's work;
- `part`: finish each part before the next, so a part that breaks free costs
  one part.

Tool order is an order of *toolpaths*, since a rough-then-finish operation is
two of them; `GenerateAll` ranks them by the same rule (`ToolRank`), and a
finishing tool ranks where its operation first appears. `splitOutput` writes one program per tool and forces tool order
(the panel says so); see [post.md](post.md#files).

## Undo

A job tab has its own undo stack (`JobHistory`, full `JobDocument` snapshots,
the drawing pattern). One `JobUndoGuard` around a mutation — every panel edit
site and every mutating RPC — pushes a snapshot when the job's fingerprint
moved, and coalesces a drag while an ImGui item stays active. The verbs, the
picks and the gizmos snapshot explicitly, one step per drag. `edit.undo` /
`edit.redo` and the `undo` RPC route to it on a job tab.
