# Posting G-code

`cam::PostProcess` (`src/core/cam/CamPost.cpp`) turns one setup's toolpaths
into one program; `cam::PostProcessSplit` into one program per tool. The toolpath
stays a polyline in setup space; everything machine-facing — the dialect,
the work origin, arcs, tool changes, the rotary feed — happens here, so the
backplot, the verifier and every test see the same moves. The post is built in
every configuration (it is part of the document model), with or without
`ENABLE_CAM`.

## Dialects

A narrow interface with one row per controller (`kDialects`), differing in
the tool change, the program end, and what the controller can do: helical
arcs and cutter compensation.

| post | tool change | end | G41/G42 |
|---|---|---|---|
| `mach3` | `T<n> M6`, then `M0 (change tool, set Z zero, then cycle start)` — manual | `M30` | yes |
| `linuxcnc` | `T<n> M6` — the controller's tool table does the rest | `M2` | yes |
| `grbl` | `T<n>` (no `M6`: GRBL ignores it unless built for a changer), `M5`, a comment naming the tool and "set Z zero, then cycle start", `M0` — a feed hold every GRBL honours | `M2` | no |

The bare `T<n>` on GRBL moves nothing and costs nothing, and it is what makes
the FILE say which cutter each move belongs to; without it every reader,
the verifier included, would assume tool 1. The drill posts plain moves, so no
dialect needs canned cycles translated. The post id is the machine's
(`Machine::post`); an unknown id is refused. A new controller is a row.

## What the file says

- **Preamble:** `G90 G91.1 G21 G17 G40 G49 G80` (without `G91.1` when arcs are
  off), then `G54`. Absolute, millimetres, XY plane, no leftover cutter comp
  or canned cycle: a preamble that assumes the machine's state is how a job
  starts in inches. `G91.1` states that arc centres are incremental; a file
  that does not say puts the centre wherever the machine was left.
- **Comments** (`opts.comments`): the setup and post, `tool N: name Dd shape`
  at each change (the shape, because `T2 D6` is the same line for an end mill
  and a ball), `(op <id>)` before each operation (which is how the simulation
  attributes moves), `(SKIPPED op N: why)` for an operation that produced
  nothing and `(SKIPPED on op N: …)` for a target it could not follow, and
  in the header `(fit op N: bore D32 K7 31.982-32.007, aimed at
  31.9945)` for each toleranced wall the program aims at, and `(fit op N:
  floor 8 from Z20 +0.300/+0.100 8.1-8.3, aimed at 8.2 [Z11.8])` for each
  toleranced floor (a `shoulder` above its reference), so the machinist
  gauges against the band ([tolerance bands](operations.md#tolerance-bands)).
  Text a
  person typed is sanitised: G-code comments do not nest, so a parenthesis in a
  tool's name would be a file the controller refuses to load; brackets stand
  in, and newlines go.
- **Cutter compensation**, per compensated op, in the header: `(comp op 7:
  G41 D6 WEAR - ...)`, what the tool table's entry holds and the wear the
  program was checked to take ([below](#cutter-compensation)).
- **Modal**: nothing is restated that has not changed.
- **Retracts touch Z alone.** A retract before anything has moved must not
  pass modal X/Y that nothing set (no `Xnan` can reach a file; a test asserts
  it). The post retracts to safe Z before every tool change, always, and at
  the end: retract, `M5`, end word.
- **Spindle** `S<rpm> M3` is restated after every pause even when the speed
  has not changed — a manual change is done with the spindle stopped.
- **Rapids** are `G0`. `Setup::rapidFeed` (seeded from the machine, and
  saved with the setup as its own `rapid_feed` since v139) is the
  traverse rate the cycle time is clocked with, and the feed of adaptive's
  links through cleared material (still `G1`, a straight line); when it is
  zero rapids are left out of the clock rather than invented, and the links
  run at the cutting feed.
- **Rotary**: an indexed setup states `G0 A<index>` at safe Z before its first
  move; an operation cut at several indices turns A on a rapid between its
  angles, and the post retracts to safe Z before any rapid that turns A
  ([rotary.md](rotary.md#one-operation-at-several-indices)); a turning cut's F is scaled for the controller and `G92 A` renames
  the angle between operations that end and start turns apart
  ([rotary.md](rotary.md#a-winding-g92-a)). A setup held in the rotary is
  written in the machine's frame: X/Y through the rotary's quarter-turn, the
  angle signed and lettered as the machine's `rotary` says, and refused when
  it does not say ([rotary.md](rotary.md#how-the-rotary-is-installed)).

## Work origin in the file

Every X, Y and Z the `Emitter` writes has `Setup::origin` subtracted — the one
place the subtraction happens. Zero in the file is the corner the panel names
(stock corner, stock centre, part corner, or the far stock corner) at the
height of the picked top face, so the operator touches off **on that corner,
on the face that was clicked**, and sets the offset to zero. Arc centres are
incremental, so the offset cancels in `I`/`J` (a test compares them rather
than trusting that). Readers of a posted file — the verifier and the simulator
— add the origin back (`ProgramInSetupSpace`, the inverse of what `Emitter`
subtracts); a test asserts a job zeroed on its corner still plays its own
verdict, and that reading the file raw disagrees.

## Arcs and line merging

Offsetting flattens every curve, so arcs are fitted back **at post time**
(`fitArcs`, a machine property): the toolpath stays a polyline and only the
post and the parser know arcs exist. A run fits when it is at least three
cut moves at constant Z, constant feed and constant A (so indexed jobs fit;
sloped rasters and turning cuts do not), and every point and every chord's
bulge lies within a quarter of the 0.02 mm chord tolerance the section used —
a fit tighter than the polyline's own approximation recovers arcs rather than
inventing them across a corner. The circle is a least-squares fit over the
whole run (three adjacent tessellation points are ill-conditioned), then the
centre is slid onto the endpoints' perpendicular bisector so the two radii
match exactly, which is what LinuxCNC checks before it refuses an arc. Only
flat `G2`/`G3` in G17 are written; no helical arcs.

Where the arc fitter declines, runs of collinear moves collapse to their
endpoints within the same tolerance (`LastCollinear`, fully 3D), stopping
before any arc so a tangentially-leaving arc keeps its first points. With
`fitArcs` off this is what keeps an adaptive pocket's file a manageable size.
The verifier parses `G2`/`G3` into chords, so the check is exact for what it
sees; posting with arcs on and off must give the same verdict.

### Helical arcs

A path that is made of arcs says so: a thread mill declares each of its arcs
(`Toolpath::Arc`: the moves that are chords of it, the centre, the
direction), and the post writes each as ONE `G2`/`G3`, with a `Z` word when Z
moves along it -- a helix, which every dialect here interpolates (Mach3's
helical interpolation, LinuxCNC's third axis linear along the arc, GRBL 1.1;
the `helical` column of `kDialects`, `PostWritesHelicalArcs`). The declaration
is checked before it is trusted (`ExactArc`): every chord on the circle,
turning one way, Z linear in the angle, one feed and one A, short of a full
turn. A run that a clip or a merge has changed since is written as its
chords, which cut the same. A declaring path is not run through the fitter,
which knows only flat arcs and would take a flat lead-in and the first chords
of a helix for one circle. A thread mill is refused on a machine set to post
no arcs, and on a post without helical arcs
([operations.md](operations.md#thread-mill)).

## Cutter compensation

An operation can post its finished walls under cutter radius compensation
(`Operation::cutterComp`, `cutter_comp`: `off` | `wear`, on profile, pocket
and drill), so a wall the machine cuts off size is corrected at the machine:
gauge it, type the error into the tool table, run the pass again. The
motivating case is the pan/tilt base plate's Ø47 K7 seat, 25 µm wide on a
router whose runout alone takes half of that
([generators/pantilt](../../generators/pantilt/README.md#the-base-plate-on-the-6090)).

**Wear mode, only.** The file is the tool centre, exactly as without
compensation, and goes under `G41`/`G42 D<tool>`, so the controller's tool
table holds only the WEAR, not the cutter's radius. The other convention, "in
control" (the part contour posted, the table holding the whole radius), is
not offered:

- the verifier, the playback, the holder and fixture checks and the band
  judgment all read the posted file ([one engine](simulation.md#one-engine)).
  A wear-mode file is the cut they read; with no wear entered it cuts what was
  verified. An in-control file would need a second implementation of the
  controller's offsetting to know where the tool goes, and its disagreements
  with Mach3's would be invisible;
- a router's tool table is usually empty. In control, an empty entry cuts
  every wall a radius into the part; in wear mode, the verified program.

**The sign.** `G41` (tool left of the path) when the wall is on the right of
travel — climb — and `G42` when it is on the left, so in both a POSITIVE entry
moves the tool away from the wall: the table's diameter is the error to take
off. +0.02 leaves 0.01 more on each wall (a bore 0.02 smaller, a boss 0.02
bigger); a negative entry takes more (RS274/NGC controllers swap the side for a
negative diameter). A wall cannot be put back, so a bore that comes out over is
cut first with a positive entry and gauged. The D word is the tool's number.

**What is compensated.** The strategies declare each pass that follows a
finished wall (`Toolpath::Wall`, `DeclareWall`): a profile's laps, a pocket's
wall rings (level 0, islands too), a bore's wall ring (its helix and the flat
turn). On a rough-then-finish op the finish only — the rough's wall is the
finish's stock. `CompensateWalls` (`CamComp.cpp`) then gives each one:

- **a lead-in**: a line half the cutter's radius long, then a quarter arc of
  half its radius tangent into the wall pass's first point. `G41`/`G42` goes on
  its own line before the line. A wall pass starts after a rapid down to the
  rapid floor (above the stock) or by a link at depth through what the inner
  rings cleared, so the lead-in is in air or in cleared material; the rapids
  and the link are rerouted to the line's start.
- **a lead-out**: a quarter arc away from the wall, then the line, `G40` on its
  own line before it, so the controller's taper back to the programmed path
  happens away from the wall and leaves no mark. The retract rises over the
  line's end.
- **room**: every lead point on the waste side of the pass — inside the bore,
  pocket or opening it walks round, outside a part — or the op is refused,
  naming the wall. A compensated profile starts every pass at the same point
  (the lead-arc rule, [profile](operations.md#profile)) and does not stay
  down between passes, so its leads stack into the waste a stepdown each.

The run between `G41` and `G40` is written like any other moves (arcs fitted,
straight runs merged), but no arc or merged run crosses either end
(`Toolpath::Comp`, checked before the post trusts it: a clip or merge that
changed it refuses the post).

**What a controller can offset, checked.** The program is checked for a wear of
up to a tenth of the cutter's diameter either way (`CompCheckedWear`: 0.6 mm in
the table for a 6 mm cutter, far more than runout and deflection, and small
enough that an outside corner's own radius — the cutter's — still offsets).
Offsetting by w takes `w tan(theta/2)` off each segment at a turn of theta
toward the offset; a segment shorter than its two ends take, or a path that
doubles back, is where LinuxCNC refuses ("gouging") and Mach3 cuts the wrong
thing. Such a place refuses the op, in the file's coordinates: an inside
corner of the part whose radius is within that wear of the cutter's (a sharp
inside corner is a sharp corner of the tool path and compensates fine), or a
corner relief's bite.

**Refused**: on a post without compensation (`grbl`, the `G41/G42` column;
`CutterCompRefusal`, at post time, naming the op and the posts that have it);
on a profile `on` its contour (no wall to move); with `lead_arcs` (two
lead-outs); on a hole the cutter's size (a plunge has no wall); where the
leads have no room; where a turn is too tight; where the machine's travel or
the rotary's holding clips part of the run.

**The file says it** in the header, per op:
`(comp op 7: G41 D6 WEAR - tool 6's diameter in the tool table is the wear
alone: 0 cuts as programmed, +0.02 leaves 0.01 more on each wall; checked to
+-0.6)`, and the setup sheet says how to use it
([setup.md](setup.md#the-setup-sheet)). The verifier and the playback simulate
the nominal tool — the file with no wear — and say so
([simulation.md](simulation.md#the-verifier)).

## Tool changes

A tool change is emitted on every change of tool number in the order the
toolpaths come (see [setup.md](setup.md#cutting-order-and-split-output) for
ordering). The operator's pause is the dialect's (`pauseOnToolChange` on
Mach3). The index for an indexed setup is stated once, at safe Z, before the
first operation.

## Clipping to the machine

When the machine states a work area, `GenerateAll` clips every toolpath to it
before the post sees it (`ClipToMachine`,
[setup.md](setup.md#the-machines-work-area)), so the file stops where the
machine stops and `cam_post`/`cam_toolpath` report `clipped_mm`.

## Files

- **GUI (Save G-code, in the Generate window):** written beside the project
  bundle — never inside it, where it would change the project's disk stamp
  and make the next save refuse — named `<project> - <setup>.nc`, or with
  split output `<project> - <setup>-T<n>.nc` per tool. An unsaved project is
  refused ("save the project first"). Posting is refused while any toolpath
  is pending.
- **RPC:** `cam_post {job?, setup, path, post?}` writes `path`; with split
  output `path` is the base and each tool's program is
  `<base>-T<n>.<ext>` (no spaces: these go on an SD card and through a
  shell), listed in `files`. It reports lines, skipped operations and targets,
  `clipped_mm`, `cutting_minutes` (rapids only when a traverse rate is
  stated) and `warnings`. It writes a file; it does not send it to a machine.
- **Split output** (`Setup::splitOutput`): one complete program per tool, each
  with its own preamble and end and starting by loading its tool, for
  controllers that cannot take a change mid-file. It forces tool order; a tool
  used twice with another between is refused.

## At the machine

What the verifier cannot check for you, in the order it matters:

1. **Z zero.** The file is relative to the work origin: touch off on the
   corner the panel names, at the face you picked as the top, and set the
   offset to zero. The panel prints the origin triple next to the corner
   selector; that is what the machine should read at the touch-off point.
2. **The first rapid.** Safe Z is derived over the stock and the modelled
   fixtures; a clamp that is not modelled as a fixture is invisible to it.
3. **The tool change.** After `M0`, the spindle restarts with `S… M3`.
4. **Tabs** hold the part at their stated height; the backplot draws them.
5. **Slugs** from through openings fall out; tab them separately if that is
   not acceptable.
6. **A rotary job's feed model**: confirm with one timed move
   ([rotary.md](rotary.md#feed-on-a-turning-cut)).
7. **A compensated op's tool table entry**: the wear alone, never the
   cutter's diameter ([cutter compensation](#cutter-compensation)). The
   header names the entry; set it before the first run.

## Goldens

`tests/golden/cam-block-{mach3,linuxcnc,grbl}.nc` and
`cam-curved-{mach3,linuxcnc,grbl}.nc` are compared byte for byte
(`test_cam_machine.cpp`): substring checks prove pieces are present, only a
golden proves the file stopped changing. `DESIGNER_REFRESH_CAM_GOLDEN=1`
regenerates them, and the diff is read before it is committed — every line
of those files eventually moves a machine.
