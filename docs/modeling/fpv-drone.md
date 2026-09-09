# A 5-inch FPV freestyle quadcopter

`generators/fpv_drone.py` builds a complete, buildable 5" freestyle quad:
five carbon parts to have cut, two to print, forty-two fasteners derived
from the holes they go in, and every bought component at its catalogue
dimensions so that the clearances mean something.

    python3 generators/fpv_drone.py --port <n> --stage all --out scratch/fpv-drone

Roughly 12 s for the model, 40 s more for the checks, and about a minute
for the sheets, the exports and a 256-sample render. `--stage drawings`
reopens the saved project and does the sheets alone in 21 s, which is the
loop worth having: this generator was run 90 times while it was written,
and 40 of those were a full rebuild-and-verify to look at one sheet.

## What is real and what is an envelope

| kind | parts | modelled as |
|---|---|---|
| made | bottom / mid / top plate, arm, camera plate | full geometry, dimensioned sheets, STEP |
| printed | antenna mount, battery pad | full geometry, STL |
| hardware | 42 screws, nuts and standoffs | generated from the ISO tables |
| bought | motors, props, stack, camera, VTX, RX, battery, capacitor, buzzer, antenna | outside shape + **mounting features** at catalogue dimensions |

An envelope is not a guess: a motor carries its 16 x 16 tapped holes at
their real depth, the boards carry the 30.5 mm stack pattern and a 20 x 20
VTX pattern, the camera carries the M2 tilt taps a 19 mm micro camera has.
Those are the features the frame is cut to fit, so they are the ones that
have to be right. What is *not* modelled is anything internal: windings,
PCB layers, cells.

## The layout, and why it is that

Everything below follows from three numbers: a 220 mm wheelbase, a 129.5 mm
propeller and a 6S 1300 pack.

- **The top deck sits under the propeller disc.** The props run at z 32-38;
  15 mm standoffs put the top plate at 25-27, six clear. This is what sets
  the standoff length, not the other way round — and it is why the camera
  cannot fit between the decks and looks out through a notch in the top
  plate instead.
- **The battery is between the props, not under them.** A 72 mm pack with
  its rear corners at x -47 clears the rear discs; its lead comes out the
  BACK, because out the front it is 14 mm inside the camera cage.
- **The camera cage assembles straight down.** Each side plate has one tab
  through a slot in the mid plate, landing on the bottom plate; two M2
  screws into a tapped spacer lock the pair together. The slot is 1.2 mm
  longer than the tab at each end, which is where the tab's root radius
  goes.
- **The antenna is on the centreline.** Set out to one side at the rear it
  is inside the rear propeller's disc — at the height where the stem
  crosses the prop plane, anything past y = 13 is inside it.
- **The arm roots stop at r = 30.** Not for the arms' sake but for what is
  inboard of them: the stack's spacers, and the four nuts under the mid
  plate that hold the video transmitter.

## What the run asserts

`verify()` refuses to save anything it has not checked:

- every part builds as one valid solid (`check(solids=1)`);
- the exact interference sweep over 91 occurrences is **empty** — not "small",
  empty;
- `check_fasteners` re-derives all 42 screws against the stacks they are in
  and reports nothing;
- seven clearances, measured not calculated: prop to prop 30.7, prop to top
  plate 37.1, prop to battery 33.8, prop to antenna 14.3, battery to camera
  7.4 mm;
- a motion study turns all four propellers through a blade pitch (the four
  revolute mates are coupled by chain and gear relations, so one driver
  turns the lot) and no pair comes into contact that was not touching at
  rest;
- and the aircraft balances where it should: -7.6 mm of the middle of the
  propeller square fore-and-aft (slightly tail-heavy, which is what a
  freestyle quad wants) and +0.4 mm across. That is re-weighted from the
  catalogue masses, not read off the document — a battery and four motors
  are most of the mass and all five are envelopes.

Mass is reported honestly rather than read off the document: the carbon is
carbon (the material table grew a 1550 kg/m3 entry for it), and
`mass-budget.csv` replaces every BOUGHT row with its catalogue mass,
because a battery and four motors are most of the weight and all five are
envelopes. 435 g dry, 654 g all-up, about 9:1 thrust to weight.

## What building it found

Eight defects, all fixed, and six things the app or its SDK simply did not
do. Every defect fired on an ORDINARY joint or an ordinary sheet, which is
why they had survived.

1. **A through hole in a thin plate warned that it drilled air.** The
   check sampled the bore over its own length, and a through bore is a
   bounding-box diagonal long: nine samples spread over 140 mm all miss a
   3 mm plate. Now clipped to the bodies' own box. (`HoleFeature`)
2. **A far-end pick did not stop a joint being "blind".** `JointEnd` read
   blindness off the axis alone, so a nut was refused on any through-bolt
   whose line carried on into another part — an arm bolt with its nyloc on
   the mid plate, refused because the top plate is 15 mm further up.
   (`AxisStack`)
3. **The tapped-hole check asked the wrong part.** A screw that passes
   through clearance holes into a tapped standoff was refused because the
   hole it *enters* is, correctly, a clearance hole. It now asks the part
   the axis stops in. (`FastenerActions`)
4. **The thread table started at M3.** An FPV stack, a 19 mm camera and a
   20 x 20 video transmitter are all M2. Added M1.6, M2, M2.5, which the
   clearance-hole table beside it had all along. (`ThreadStandards`)
5. **A generated screw did not declare its own thread**, so every screw in
   a tapped hole read as a clash of exactly the engagement volume — 24 of
   them here. A screw now carries an external thread region on its shank.
   (`FastenerFeature`)
6. **Thread relief depended on the argument order.** The same screw and
   spacer came back clean as (screw, spacer) and 6.8 mm3 of clash as
   (spacer, screw), and a whole-assembly sweep picks the order. Now
   measured in the external thread's frame. (`Measure`)

Two more came out of a second pass over the sheets:

7. **A hole callout could never be auto-placed.** `add_drawing_dim` says
   "omit it to auto-place clear of the view", and it does that for linear
   dimensions — but the placement search re-resolved the dimension without
   a source document, which a callout needs to resolve at all, so every
   callout fell through to a fixed offset 12 mm from its own hole. On the
   part. The workaround was to compute every label position by hand, which
   is what this generator did for a day. (`DrawingDimension`,
   `DrawingActions`)
8. **The general arrangement's own views overlapped**, which was this
   generator's fault twice over: three hard-coded positions at 1:1 on a
   sheet the aircraft does not fit on, and no `check_drawing` on that
   sheet — the gate that every part sheet ran, and that reports view
   overlap by name. Laid out from measured extents now, on A3 at 1:2, and
   gated like the rest. A view's `position` is where its model ORIGIN
   lands, not its drawing's centre: a side view that runs from z 0 upward
   sits 18 mm above the point that places it.

9. **A drawing note with newlines in it** was neither measured (`LabelBox`
   boxed the whole string as one line, so a 200-character hole table came
   out 440 mm wide and reported as running off the sheet) nor rendered (the
   SVG wrote one `<text>` element, which ignores newlines; the PDF one text
   object). All three per line now. (`DrawingDimension`, `DrawingExport`)

And six gaps, now filled: **carbon fibre** in the material table;
**`check_drawing`/`update_drawing` take a `drawing`** rather than judging
whichever sheet is active; **interference pairs carry `a_id`/`b_id`**,
because sixteen occurrences answer to "ISO 4762 M3x10"; **a motion study
reports `seated_contacts` and `appeared`**, since a mesh check calls
touching a clash and a working machine is never "clean"; a **trailing
underscore** in the SDK reaches a parameter python reserves (`from_`); and
two doc strings that cost real time — `add_mate`'s `flipped` DEFAULTS TO
TRUE, and `mass_properties` returns `mass_g`.

## Where the time goes

`--stage all` reports it. 57 s, of which:

| | s | |
|---|---|---|
| fasteners | 14.5 | 42 `fit_fastener` calls, each traversing an axis through 91 occurrences |
| renders | 11.9 | a 256-sample path trace and a view sheet |
| fastener check | 8.7 | all 42 re-derived against their stacks |
| clearances | 5.9 | six exact distances |
| parts | 3.5 | 25 parts, 300-odd features |
| everything else | 12 | clash sweep 1.4, motion study 1.4, sheets 1.6, exports 1.9 |

The motion study — 24 poses, every pair checked at each — is 1.4 s, and the
whole-assembly exact clash sweep is 1.4 s: box pruning does its job.

One measurement used to cost 55 s of that: the diagonal propeller pair. An
exact minimum distance prunes face pairs by their boxes against the best
found so far, and between two lofted propellers 151 mm apart every facing
pair sits in the same narrow band, so nothing prunes and 900-odd
spline-spline extrema get computed. It is now 40 ms, and the story of how
is worth keeping because two of the three obvious fixes did not work.

**Bounding it by the shapes' vertices**: no. Measured, a 153.4 bound against
a true 151.45 still left 30 of 53 faces live on each side — a twisted
blade's bounding box is enormous next to the gap being measured.

**Pruning at triangle granularity, then refining exactly**: yes, but not
here. `collide::Mesh` grew a distance traversal — a dual-BVH walk with
box-distance pruning and a triangle-triangle primitive — that reports the
distance AND which B-Rep face pairs could hold it, nearest first. An exact
query over those pairs is exact (a tessellation lies within its deflection
of its surface, so the true pair's triangles are within `mesh + 2
deflections`) and much faster on ordinary parts: propeller to camera plate
3.3 s to 0.6 s, camera to battery 7 ms. But two propeller blades run nearly
PARALLEL, so 177 face pairs are equally close, and refining 177 spline pairs
at 0.4 s each is worse than asking OpenCASCADE once — 69 s against 17. Where
the list is short the refinement wins outright: those same propellers 151 mm
apart give 32 candidates, 4.5 s refined against 51 s whole-shape. So
`Measure` caps refinement at 64 pairs, which is a cost estimate (N candidates
cost about N face-pair extrema) rather than a limit, and hands longer lists
back.

**Not asking for an exact number at all**: `measure` grew `mode: "mesh"` —
the same bargain `interference` has always offered. 10 ms against 51 s for
that propeller pair. It reports `tolerance_mm`, the bound on how far it can
be from the exact answer, and the generator SUBTRACTS that rather than
trusting the figure flat, which is why the clearances above read as "at
worst". The bound is the two meshes' deflections SUMMED, not the coarser of
them: each tessellation can err its own way. Measured across this assembly
the real error is 0.000 to 0.054 mm against bounds of 0.29 to 0.62 — and
zero wherever a flat face is involved, because a plane tessellates exactly.

And the last of it was a plain mistake, found by measuring the default after
claiming it was faster: `measure` never used the path that had been
optimised. Its distance goes through `DistanceBetween`, which asked
BRepExtrema about the fused compounds, while the refinement had gone into
`ContactBetween` (which is what the interference sweep uses). Worse, the
handler called BOTH — `OverlapBetween`, which exists for the overlap volume,
was computing a distance nobody read. With the refinement in the right place
and the second distance gone, the EXACT default is:

| between | was | now |
|---|---|---|
| propellers, 151 mm apart | 51 s | 4.2 s |
| propellers, 30 mm apart | 25 s | 13.5 s |
| propeller to camera plate | 2.4 s | 0.6 s |
| battery to camera | 0.02 s | 0.02 s |

all still exact. Mesh mode is the further 100-400x on top of that, for when
a clearance is what is wanted rather than a coordinate.

## What happens to the parts

`generators/fpv_drone_shop.py`, `--stage fem|cam|shop`, against the project as
saved. Neither half touches a part: they add STUDIES and JOB DOCUMENTS, and
the project is saved with them. The G-code is not the answer on its own — the
nesting, the tools, the depths and the load cases are what somebody opens to
change one of them, and a `.nc` file cannot be edited back into a job. Saved
and reopened, the three jobs post the same nine files byte for byte.

The one thing either half writes outside the project is the machine: a setup
names one from the store (`~/.config/designer/machines`) and then EMBEDS a
copy, so the job opens anywhere and says `machine_match: missing` on a machine
that has never heard of this router rather than pretending.

### What the parts carry

Three static studies, 41 s between them, on the two parts that carry a load —
the arm, twice, and the top plate.

| | elements | movement | peak | factor | where |
|---|---|---|---|---|---|
| arm at full thrust | 13903 | 0.11 mm | 26.2 MPa | 21.0 | outer arm bolt |
| arm on a 15 g landing | 13903 | 0.70 mm | 171.6 MPa | 3.2 | outer arm bolt |
| top plate under the pack at 15 g | 8968 | 0.02 mm | 15.3 MPa | 35.8 | strap slot |

The load worth asking about is not thrust. One 2207 on 6S pulls about 1.5 kgf
and the arm barely notices — a factor of 21. What designs a 5 mm arm is
ARRIVING: a quadcopter lands on one corner and the deceleration is set by how
far the structure gives, which is millimetres, so tens of g. 15 g through one
arm is the conventional freestyle case and it is where the factor drops to
3.2. Both peaks are at the OUTER arm bolt hole — which is where a real
freestyle arm cracks, so the model is at least failing in the right place.

The arm's peak STRESS and its peak MOVEMENT are not in the same place: the
stress is at the bolt hole (54, 0, 0), the 0.70 mm of deflection is out at the
motor (126, 0, 0), which is the end that is free to move.

A solved result is derived and the project does not keep one — only a topology
study's density is written — so the reopened studies carry their load cases,
mesh size and fixings and report `has_results: false`, `stale: true`. That is
the right call (a stress field that outlives the geometry it was computed from
is a trap), and it is why the numbers are written to `fem.csv` beside the
model as well as to the terminal. Re-running them is 25 s.

**Read them as indicative, not qualifying,** and the run prints the same
caveat beside the numbers (it is the last row of the CSV too): the solver is isotropic and a carbon laminate is
not. In-plane it is a fair proxy — quasi-isotropic 3K/epoxy, in-plane loads —
but a real arm fails by DELAMINATION at the root, and no isotropic model can
see that.

### The G-code that cuts them

One job per sheet thickness, because that is what a sheet is: 320 x 320 mm of
plate, a 600 x 400 hobby router, four cutters.

| sheet | parts | ops | cutting | time | lines |
|---|---|---|---|---|---|
| 3 mm | bottom plate | 3 | 1.5 m | 5 min | 385 |
| 2 mm | mid, top, 2 x camera plate | 5 | 4.1 m | 15 min | 2105 |
| 5 mm | 4 x arm | 6 | 5.6 m | 14 min | 2035 |

Every sheet: **no gouge, and no rapid through material**, checked by
simulating the POSTED file back against the model (`cam_verify`) rather than
the toolpath that produced it. The time is the file's own feeds; no machine
states its rapid rate, so it is time CUTTING and not an estimate of the job.

What decided the shape of it:

- **Four cutters, and the parts chose them.** Carbon is drilled, not
  interpolated, at these sizes — a 2 mm cutter would be in a 3.2 mm hole for
  minutes and blunt before it is out — so the M3 and M2 holes are a 3.2 and a
  2.2 mm drill. The narrowest cut on the aircraft is the 2.2 mm slot a camera
  plate drops through, so nothing wider than 2 mm can cut the slots, and the
  outline and the big openings take the 3.175 mm diamond-cut.
- **Order is not cosmetic.** Drill, then open up, then cut free: a part cut
  free first moves. And with one program per tool (`split_output`, which a
  manual tool change wants) the tools run in the order they are first WANTED,
  so the outline has to be the last thing asked of the big cutter — otherwise
  the parts are free before their slots are cut, in a different file.
- **The camera plates are cut lying down.** They are modelled upright, where
  they live in the aircraft; `lay_face_up` clamps each one with its own face
  at the spindle. That is a statement about the fixture, not the machine — no
  A word is posted — and it is what lets one sheet hold three plates on their
  faces beside a part modelled standing up.
- **Nesting is read back, not re-derived.** Where each part sits comes from
  its box in SETUP space, which `check_layout` reports because it had to
  measure it to answer whether the part is on the sheet. Working the same
  rotation out again in the generator is how two copies of a transform come
  to disagree.
- **The openings are the model's, not a list.** One operation per part cuts
  every opening in that part's own face; `drilled_bores` leaves the round
  ones the drills take. The generator used to carry a table of every
  opening's position, restated from the spec — the one place where a spec
  change could stop cutting a slot in silence.
- **What is left standing is now small enough to read.** The toolpath tells
  the verifier where it left each tab, so a tabbed job can be `complete`;
  what remains is the offcut the outline cut through (1, 4 and 16 regions of
  about 4 mm2). `ok` is still the verdict to gate on — nothing scraps the
  part or breaks the tool — and a through cut's slug is still invisible to a
  height map.

`--stage fem` also saves a plot of each field beside `fem.csv`. It is worth
more here than it would be elsewhere: the solve is not persisted, so the
picture and the CSV row are what survive the run — and the arm's plot puts
the red exactly where the number says it is, at the outer bolt hole.

### What this half found

Five defects on the first pass, all fixed, and every one of them a case that
only shows up when a job holds more than one part or more than one tool:

1. **A grbl file never said which tool it was running.** GRBL takes no M6, so
   the tool change is a message and a pause — and the message was the only
   thing naming the cutter. Nothing that reads the file back could tell, so
   everything after the first change was read as tool 1: the verifier
   simulated a 2 mm cutter's slots with the 3.175 mm one and called a correct
   job scrap, at exactly the depth of the plate. The T word costs nothing at
   the machine (GRBL parses it and moves nothing) and is what makes the file
   self-describing. (`CamPost`)
2. **A drilled through hole did not break through.** The derived depth was the
   part's bottom face, and a drill is a cone: its point reached the far side
   while the full diameter stopped short, leaving a ring of material the shape
   of the point. The verifier has always modelled the point — `TipHeight` is
   one function — so the two disagreed, and correct drilling read as 1.2 mm of
   material left standing in every hole. A through bore now goes on down by
   the point's own height; a blind one keeps its floor, because there the cone
   IS the bottom of the hole. (`CamDerive`)
3. **A drill retracting up its own hole was a "rapid through material".** The
   rapid check treated every tool as a cylinder while the cut used the tool's
   shape, so the cone the drill had just made read as material in the way.
   Every through hole in a job reported one — the kind of false alarm that
   teaches an operator to ignore the real one. Both sides use `TipHeight`
   now. (`CamVerify`)
4. **A pick could not name which part of the job it was on.** Feature ids are
   per document, so a mid plate and a top plate on one sheet share them
   freely; every geometry pick took an element ref alone and answered for the
   first body with that id. `lay_face_up` on the second camera plate was
   refused outright ("needs a FLAT face on this part"), and an inside profile
   on the top plate's strap slot came back "that face is gone from the model".
   The qualifier an operation target has carried since v76 now reaches the
   pickers themselves. Silent-wrong was the other half of this: where the name
   HAD resolved on the wrong part, the cut would have been made on it.
   (`CamPicks`, `CamSetup`)
5. **`add_cam_op` documented fourteen parameters it would not accept.** The
   handler reads them all — through a helper the params gate's scanner cannot
   follow — so the row beside it listed only the eight read in the branch
   itself, and the generated SDK refuses anything not in the row. `tabs` was
   documented in prose and unreachable: no script could ask for a tab. The
   gate has an escape hatch for exactly this (`// rpc-params:`), and it now
   carries the full list. (`RemoteCam`)

And five more from a second pass, whose first act was to write the test that
would have caught the first five — one sheet, two parts sharing feature ids,
two cutters, posted and verified:

6. **Every job with a tab reported `complete: false`.** The flag is
   `maxLeftover` against one cell of tolerance, and a tab is at least 0.5 mm
   of full-height material asked for on purpose — so the flag said nothing
   about any real job, and the docs explained it away instead. The toolpath
   now records where it left each tab (position, the pillar's radius, and its
   top) and cells at or below that top stop counting as leftover. Unlike a
   corner relief this needs the HEIGHT: a relief excuses a cut that went too
   deep, a tab excuses one that did not happen, and excusing it by position
   alone would excuse an uncut slab above it. Reported as `tabs` rather than
   silently subtracted. (`CamStrategies`, `CamVerify`)
7. **An inside profile on a level face cut a trench round the part.** A face
   pick handed the profile the face's OUTER wire, so `side: inside` offset
   the part's own outline inward and cut it full depth, all the way round —
   the exact failure the refusal three lines below describes, which never
   fired because it only tests for an empty contour list. It now cuts the
   face's OPENINGS, which is the only thing an inside cut on a face can mean,
   with `drilled_bores` to leave the round ones the drill takes (v96). A
   width could not say that: the narrowest MILLED opening on this frame is a
   2.2 mm slot and the widest DRILLED hole is 3.2 mm across. (`CamStrategies`)
8. **A through hole's bottom was the STOCK's, not the model's.** The
   break-through from the first pass measured the part's bottom as the
   stock's plus its bottom margin — true for relative stock, where the stock
   IS the model plus that margin, and wrong on a fixed sheet, where a 4 µm
   bounding-box difference then decided whether the hole broke out. The
   drone's arms stopped breaking through when a margin changed elsewhere, in
   silence. (`CamDerive`)
9. **A job could only be addressed by whichever tab was in front.** All
   seventeen job RPCs went through the active job; `cam_post` writes a FILE,
   so a tab switch between calls puts another job's program under the name
   you asked for. `job` names it, exactly as `drawing` does for a sheet.
   (`RemoteCam`)
10. **`viewport_click` and `viewport_drag` advertised their modifiers and not
    their coordinates** — x/y go through a helper, and the params gate
    compares a row against what its scanner can SEE. So the generated SDK
    refused them: every click a script could ask for landed in the middle of
    the viewport. The gate now refuses a branch that hands its params to a
    helper without declaring what the helper reads, which is the shape of
    mistake that hid fourteen `add_cam_op` parameters. (`rpc_params_scan`)

## Files

`scratch/fpv-drone/` after a run: the project, five dimensioned part sheets
(SVG + PDF) with hole tables read off the model, a general arrangement,
STEP for the assembly and for each carbon part, STL for the printed ones,
`bom.csv`, `mass-budget.csv`, and renders. `BUILD.md` beside them is a copy of
[the build guide](fpv-drone-build.md) — the build order and the buying list.

`--stage fem` adds `fem.csv` — the load cases, what each did, and the caveat
— and a plot of each. `--stage cam` adds the G-code: `<sheet>-T<n>.nc`, one
file per cutter, each complete and starting by loading its own tool. Both are
part of `--stage all`, so one command produces everything; either runs on its
own against the saved project.
