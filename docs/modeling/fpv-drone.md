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

What cost 55 s until it was removed was ONE measurement, the diagonal
propeller pair. An exact minimum distance prunes face pairs by their boxes
against the best found so far, and between two lofted propellers 151 mm
apart every facing pair sits in the same narrow band, so nothing prunes and
900-odd spline-spline extrema get computed. Bounding it by the shapes'
vertices does not help (measured: a 153.4 bound against a true 151.45 still
leaves 30 of 53 faces live, because a twisted blade's box is enormous next
to the gap). What would help is pruning at triangle granularity — a mesh
distance with a stated deflection, which `interference` has and `measure`
does not. The measurement itself was redundant: those two propellers cannot
touch, the adjacent pair is measured, and the motion study checks every
pair anyway.

## Files

`scratch/fpv-drone/` after a run: the project, five dimensioned part sheets
(SVG + PDF) with hole tables read off the model, a general arrangement,
STEP for the assembly and for each carbon part, STL for the printed ones,
`bom.csv`, `mass-budget.csv`, and renders. `BUILD.md` beside them is a copy of
[the build guide](fpv-drone-build.md) — the build order and the buying list.
