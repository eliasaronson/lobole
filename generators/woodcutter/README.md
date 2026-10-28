# woodcutter

A hand-cranked automaton in a printed box: one turn of the crank is one
chop. The axe cam lifts the woodcutter's arms slowly through a rod and a
link and lets them drop onto the log; the bird's cam, on the same shaft,
makes the bird peck while the axe is rising. Everything is printed and
nothing is bought: the box is five panels pegged together, the camshaft
turns in printed bushings, and a crank on its end turns it. This is the
automaton designed and checked in the app, its loaded parts studied on
pins, laid out to print, drawn and billed (phases 2 to 5 of
[the plan](../../docs/plans/woodcutter-automaton.md)); its film and the
print itself are the plan's later phases.

```sh
tools/headless-preview.sh --start 8800
python3 generators/print_coupon.py --port 8800 --seed   # the P1S profiles
python3 generators/woodcutter/build.py --port 8800 --profile "K5 PLA on P1S" [--width 120] [--slice] [--no-render] [--fresh]
```

The profile is required and must state its layer and line width: the
followers' check weighs the parts as they print, and the built-in table is
no printer. Output (untracked) goes to `scratch/woodcutter/`:
`woodcutter.lob`, the receipt `woodcutter.json` (every number below, from
the app), the print job `woodcutter_job.3mf`, and quick stills: at rest,
the bird pecking, the axe raised, the raised axe from the side, and each
print plate; the bill of materials `woodcutter_bom.csv`, and the sheets as
PDF and SVG: `woodcutter_assembly`, `woodcutter_axe_cam` and
`woodcutter_bird_cam`. `--no-render`, or a headless instance, skips the stills.
`--slice` slices the job with your Bambu Studio CLI (the project config
made by `tools/bambu-config.py` from its P1S presets) and reports its time
and filament.

Files: `woodcutter_spec.py` (every size and both motions, no app),
`woodcutter_parts.py` (one function a part, and the way each prints),
`woodcutter_fem.py` (the studies of the camshaft, the crank and the
handle's pin), `woodcutter_drawings.py` (the exploded view, the sheets and
the bill),
`build.py` (the project, the checks, the controls, the sequence, the
plates, the receipt), `test.json` (the
[generator test](../README.md#the-big-generators-as-tests)).

## The mechanism

- **The box** is five printed panels: the floor, the back, two end walls
  carrying the bushings' bores, and the top with a guide boss under each
  rod and the figures' slots through it. It is open at the front, so the
  cams show. The panels are pegged at their edges (below).
- **The camshaft** turns on a revolute in the left wall. It is flat on one
  side its whole length: it prints lying on that flat, and the cams, the
  bushings and the crank are pressed onto it by D bores over it. Each cam
  is a `CamProfile` placed on a Frame (Z the shaft, X up at rest), its own
  body.
- **The bushings.** A journal with the flat on it would drop by the flat's
  depth each time the flat came round under the load, so each end of the
  shaft is pressed into a printed bushing, round outside, which turns in
  its wall's bore. The bushings' flanges outside the walls hold the shaft
  along its axis.
- **The crank** is a plate pressed on the shaft's -X end outside the left
  bushing, one chop a turn with no gears. Its grip turns on its own
  revolute, on a headed pin pressed into the crank.
- **The axe cam**: a cycloidal rise of 15 mm over 210°, the drop, and a
  150° dwell for the axe to land in. **The bird's cam**: a dwell to 25°, a
  cycloidal rise and fall of 8 mm over 85° each, a dwell. So the bird pecks
  from 25° to 195° while the axe climbs, and is done before the drop at
  210°.
- **The followers** are rods with a foot across the cam: a cylinder of the
  rod's radius, so each cam is cut for a knife of that tip radius (3 mm at
  120). The foot is as wide as the rod (and a nozzle's width more: below),
  not the cam's whole width. Each rod is a D, flat on -X: it prints lying
  on the flat, and its guide is a D bore, so the rod cannot turn and its
  link stays square to the shaft. Each slides in its guide on a slider mate
  and is driven by a `contact` relation, `toward` from gravity (negative:
  it falls onto its cam).
- **The figures.** Each rod's top carries a pin through a link, and the
  link's other end a pin into a lever: the woodcutter's arms, turning on a
  shoulder pin in the figure, or the bird, turning on a pin through its
  body in a perch. Rod, link and lever with the slider make a mate loop,
  which the loop solve closes. The figure, the perch and the log stand in
  slots through the top, on a tab (the log on a rib along its underside).
  The bird's body hangs behind and a little below its pin, so its weight
  keeps its tail on the link through the whole peck.
- **The pins** are parts of their own, pressed into one part and turning in
  the other: the rod's into the rod, the link turning on it; the lever's
  into the lever; the pivot's into the figure's body. A pin built onto the
  rod could never pass its guide, nor a lever's pin go home while the
  lever goes onto its pivot. Each part between two others is held along
  the pin by its neighbours (the link between the rod and the lever, the
  lever between the link and the body), so no pin has a head but the
  crank's. Every pin and peg prints lying on a flat at its 45-degree chord,
  on the side its joint's load is not.

### Where every size comes from

From `--width` W (the box's inside width along the shaft, 120 by default),
the print's gap (`print_allowance` kind `gap` at the rod's size: 0.3 mm on
K5 PLA, the whole clearance) and its press (kind `press` at the pin's
size: 0 on K5, which has no press row, so the built-in default), nothing
else in millimetres:

| size | rule | at 120 |
|---|---|---|
| panels | a peg and four lines either side (so a peg fits an edge), at least W / 33.3 | 6.2 |
| inside depth | W / 2 | 60 |
| camshaft | Ø W / 15, a flat a sixth of it deep (past the 45-degree chord) | Ø8, 1.33 |
| guides, running pin holes | the size plus the gap | Ø6.3, Ø3.3 |
| pressed holes (pins, pegs, the shaft's D) | the size plus the press | Ø3, Ø8 |
| cams | W / 10 wide, at W / 4 and 3W / 4 | 12 |
| rods and knife tips | Ø W / 20, tip its radius | Ø6, r3 |
| strokes | axe W / 8, bird W / 15 | 15, 8 |
| base radii | the least whose CamProfile reports ≤ 30° of pressure, at least the hub's (the bore and two walls) | axe 7.45 (the hub), bird 12.05 |
| shaft height | the larger cam plus W / 20 | 28.35 |
| guide bosses | three rods long | 18 |
| inside height | the highest foot at full lift, W / 40 under the boss | 71.5 |
| pins, pegs, plates | Ø W / 40; plates W / 40 thick, bodies W / 30 | Ø3, 3, 4 |
| pegs | in a blind hole a panel less four lines deep each side, a gap short of each floor | 4.6, 8.9 long |
| bushings | two four-line walls round the shaft, the flange two more and W / 60 thick; the wall's bore the bushing plus the gap | Ø14.4, Ø20.8 x 2, Ø14.7 |
| crank | a quarter of the inside depth; a plate W / 30 thick | 15, 4 |
| handle | pin Ø W / 24, grip W / 5 long and two walls round the pin's running hole | Ø5, 24, Ø11.7 |
| figures' tabs, the log's rib | W / 12 long, W / 30 wide; the panel deep | 10, 4 |
| plate spacing | W / 30 between pieces and from the bed's edges | 4 |
| figures | the arm W × 0.3 to the axe's eye, the axe's head W / 15 | 36, 8 |
| the blade over the log | W / 120 | 1.0 |
| the levers | each pin's arm from the stroke over the swing asked (arm −40° to 80°, bird −15° to 35°), the pivot over the rod at mid-swing | 9.2, 9.6 |

The base radii are searched against CamProfile's own report in a scratch
part (in 0.05 mm steps from the closed form), then the box is laid out
round the cams. The crank's quarter of the depth puts the grip 13.7 mm
over the table at the bottom of its turn, a finger's room: a third of the
depth left 8.7 mm. The arm reaches 70.1° rather than the 80° asked: the link
tilts. The motions' angles are design choices: a slow rise over most of
the turn, a dwell long enough for the drop at twice the hand speed, and a
peck clear of the drop.

## What is checked every run

The run refuses to save what fails, and every number is in the receipt.
At W = 120 on K5 PLA on the P1S:

- **Every part** is one body, `check_geometry` says verified, and
  `check_print` passes it the way it prints (`PRINT_UP` in
  `woodcutter_parts.py`): every item but the two a plate answers (on the
  bed, and where on it), since a part is modelled where it stands in the
  box. 25 parts, 44 pieces (16 pegs, two bushings, six pins). The worst
  overhang is 45.00°, the teardrops' flanks and the pins' and the shaft's
  flats.
- **How each prints**, so a load across a member runs along its layers:
  the rods, the pins, the pegs and the camshaft lying on their flats; the
  levers, links, figures, cams and the crank flat; the bushings flange
  down; the grip and the log standing; the floor flat, the back and the
  walls on their outside faces, the top on its stage. A hole that lies
  across the print (a peg's in a panel's edge) is a Hole feature printed
  `teardrop`; every other hole stands up the print.
- **The fits**: a running joint has the profile's gap (0.3 mm, 0.15 a
  side: the guides, the links and levers on their pins, the bushings in
  the walls, the grip); a pressed one the press (0 here: the bore modelled
  at the pin, which FDM prints small). Every pressed face is a printed
  press (`set_tolerance printed: press`), so each study's exact rigid
  check relieves and lists all 44 (`press_fits`).
- **The cams' reports**: axe base 7.35 (the hub, which the press bore
  narrows from phase 2's 7.45), 22.35 to the top, 26.38° worst pressure
  angle at 86°; bird base 12.05 (the closed form for a cycloidal rise
  under 30° gives 12.03), 20.05 to the top, 29.97° at 64°; both fits
  within 3e-7 mm.
- **The assembly sequence**, 30 steps, each slid home along its way in
  against everything placed before it (`check_assembly_sequence`, 15-20
  s): the floor; the back's pegs and the back; the left wall's pegs and
  the wall (along +X: its pegs go into the floor's and the back's edges);
  the camshaft with its cams pressed on, through the left wall's bore; the
  right wall's pegs and the wall over the shaft's end; a bushing on each
  end; the crank, the grip and its pin; each rod dropped onto its cam; the
  top's pegs and the top, down over the rods; the figures and the log into
  their slots; then, for each figure, its lever, the pivot pin, the link,
  the rod's pin and the lever's. A pin starts its own length from home: a
  hand puts it at its hole, and from further along it passed through the
  other figure.
- **Two motion studies**, `clash_mode: mesh` at a fifth of the gap a side
  (0.03 mm), every part of the box in them (838 pairs checked over the
  turn): one crank turn (49 samples, 7.5° apart) and the chop (41 samples
  over 200° to 220°, half a degree apart, across the drop). Both clean: no
  clash, no branch jump, every sample solved; each cam and its rod
  `touching`, the figures on the top.
- **`check_followers`** over both at 120 rpm, twice a hand's turn, with
  each part weighed as it prints: each rod touches its cam across the
  width at every sample (the deepest entry 5e-5 mm on the axe, 3e-6 on
  the bird), the realised motion is the one the cam was cut for (deviation
  4e-5 and 3e-6 mm), gravity holds each rod (`held`) up to its liftoff:
  the axe's at 152 rpm (where the rise slows near its top, the arm
  upright), the bird's at 166 (the bare rod's closed form, √(gβ²/2πh), is
  198: the bird takes some of gravity's hold). The axe's drop: 15 mm,
  falling in 108 ms (free fall would be 55: the arm carries most of the
  inertia and, near upright, little of the weight), 140.2° of dwell to
  land in, `drop_rpm` 216, so at 120 rpm it lands after 78° of turn.
- **Requirements**, as they lie in the saved project, all passing: each
  follower's `liftoff_rpm` ≥ 120 and `pressure_angle_deg` ≤ 30, the axe's
  `drop_rpm` ≥ 120 over the chop, both studies `motion_clean`, the
  assembly sequence, and the blade's exact clearance over the log over the
  turn ≥ 0.9 mm (it reads 1.000 mm, at rest).
- **Negative controls**, each of which must fail:
  - the axe's landing dwell cut to 15°: the drop lands on the next rise
    (`drop_rpm` 16.5, the requirement fails);
  - the bird's cam on the hub's radius, 7.35: 37.8° of pressure, the
    requirement fails;
  - a guide tilted 10° out of the cam's plane, and one moved along the
    shaft off the cam's width: each refused by `add_relation` as a contact
    (a barrel or face cam is not built; the two never meet).
  Each modifies the design and puts it back, so the last requirement check
  is on the design as saved.
- **The loaded parts on pins** (`woodcutter_fem.py`): static studies of
  the parts as they print, each on `pin` supports
  ([pin supports](../../docs/simulation/README.md#pin-supports)), every
  load read from `check_followers` at 120 rpm
  ([contact forces](../../docs/assemblies/relations.md#contact-forces)),
  none estimated. The worst sample of the crank turn is the one where the
  crank needs the most torque, both followers' summed: 67.5°, 0.469 N·mm
  (the axe's rod pushing its cam with 0.052 N at 24.3° of pressure angle,
  the bird's with 0.021 N at 29.5°). Each study's factor against yield is
  its receipt's claim; the stress across the printed layers
  (`layer_normal` the part's print direction) is read beside it against
  PLA's interlayer 17 MPa, and the smaller governs.
  - *The camshaft at work*: on a pin over each bushing's sleeve (the left
    one held from turning), loaded by each follower's push on its cam
    (`in_cam_part`, a remote load at the contact spread over the cam's
    width) and by the hand's push at the grip that balances their torque
    (the followers' moment about the axis is check_followers' torque to
    1 %, or the run stops). Factor 1224 against yield, 4989 across the
    layers: turning it takes almost nothing.
  - *A child leaning on the crank*: the push that turns it, 0.031 N at the
    grip (the torque over the crank's radius), times `LEAN_FACTOR` 3000:
    93.9 N, a 9.6 kg lean, straight down the crank at the top of its turn
    (a lean that turns it turns it; the one it is held against goes
    through the shaft). On the camshaft's crank end (the shaft on its
    bushings), the crank (its hub on the shaft, held from turning: the D
    keys it; pushed through its handle's bore) and the handle's pin (in
    the crank over its thickness; the grip pushing along its length).
    **None holds it** with `LEAN_NEED` 2 to spare: factors against yield
    0.48, 0.44 and 0.23 (across the layers 1.25, 1.08 and 0.71), the
    peaks on the pins' edges (1.13, 0.73 and 0.47 away from them). The
    crank end holds 10.6 N at the grip, about 1 kg, with 2 to spare: the
    handle's Ø5 pin bending over the grip's length governs (M = 1141 N·mm
    at the crank's face; 103 MPa away from the pin's edge). The run
    records this and goes on (*Not done here*); the camshaft at work, and
    every study's statics, stop it.
  - *The statics*: in every study what the pins carry together, force and
    moment (`moment_n_mm`), answers the loads' within 1 % (0.000 % on the
    shaft and the crank, 0.34 % of moment on the handle's pin, whose
    grip's push is taken at its band's middle). The two bushings do not
    share by the lever rule: the FEM's pin has no clearance, so a pin along
    a sleeve takes moment, and leaned on, the left bushing carries the
    whole 93.9 N and the overhang's 2006 N·mm about its middle (F × 21.4
    mm, to 0.2 %), the right one 1.3 N where the lever rule says 15.9. A
    printed bushing in its bore tilts instead; the bending moment at the
    left bushing, which governs the shaft's crank end, is the overhang's
    either way.
- **The print plate**: all 44 pieces on one P1S plate, packed with W / 30
  between them clear of the bed's keep-out corner, 247.8 x 247.9 x 30 mm
  in the 256 x 256 bed (the top, standing on its stage with its bosses up,
  is the tallest piece); `check_print` passes every item on the plate,
  stacking and the keep-out zone included. No panel is over the bed, so
  nothing is split (`split_for_printing` is not called; a width whose
  piece will not fit the bed is refused). `export_print_job` writes it as
  one Bambu job.
- **What it weighs and takes to print**: 146.7 g of PLA as the app weighs
  each part printed (0.20 mm Standard: two walls, four on the shaft, the
  rods, the pins and the pegs; 15 % grid), 342 g were it solid. Bambu
  Studio's CLI (`--slice`, 02.08, its P1S and Bambu PLA Basic presets)
  slices the plate in 3 h 34 min, 153.1 g and 50.5 m of filament.

Runtime: 115 s on a quiet machine without stills before the FEM, which
adds about 45 s (the negative controls are the largest share: each one
rebuilds a cam and runs a study again), 186 s with the stills and the
slicing before the FEM; 265 s under load with it. The drawings and the
bill add about 9 s.

## Drawings and the bill

`woodcutter_drawings.py` runs once the plates are laid out, and every sheet
must pass `check_drawing` with no issue or the run stops. At W = 120:

- **The bill** (`export_bom`): 25 parts, 44 pieces, every one printed in
  PLA and none bought (a bought row stops the run), each weighed as it
  prints and on its plate (`plate 1 x16` for the pegs). 146.7 g printed,
  the plates' own sum, against 342.2 g were it solid.
- **The assembly** (A1): the front, which is the open side (the app's
  `back`: the box's back panel stands on -Y, the app's front), and the
  view from the crank at 1:1 down the left; the exploded view in iso at
  2:1 beside them; the parts list on the title block, its mass as made;
  25 balloons laid out by `layout_balloons`, none crossing. Each view is
  placed from its own projected box with a ring round it for the balloons,
  so another width lays itself out.
- **The exploded view** is the assembly's own (named `drawing`, left
  inactive on the model tab), every step from the box's sizes: the walls
  W / 8 off the shaft's ends, the bushings, the crank, the grip and its
  pin W / 12 further apart along it; the top H / 3 up off the rods, the
  rods half that, the figures with their levers, links and pins H / 4 over
  the top; each peg half way between its two panels; the camshaft and its
  cams stay. The back drops H / 2 and comes forward half the depth: the
  iso looks from behind (+X, -Y, +Z), so where it stood it would hide the
  shaft and the cams.
- **A sheet per cam** (A3, 2:1): the cam down the shaft from +X, where it
  turns counter-clockwise, and from the side; its width, the bore's
  diameter and its flat; the motion as the feature has it (each
  segment's angles, law and rise) and the lift every 15° at the tip's
  centre, both before and after the axe's drop; the base and largest radii,
  the follower and the worst pressure angle. The lift is measured clockwise
  from the top, where the follower stands: the follower stands at -θ on a
  cam turning +θ.
- **The lift table is checked against the drawn outline**: at each angle
  the tip's centre stands base + tip + lift from the axis, and the outline
  the exported SVG draws must pass a tip radius from it, to half the line's
  width (0.175 mm on the sheet). It does to 0.098 mm on the axe's and 0.145
  on the bird's, the polyline's chords on the B-spline. Left out: the
  drop's angle and the 16.8° after it, where the tip, a tip radius wide,
  still meets the step's face (it is falling there, as check_followers
  leaves the drop's corners out).

## What the app needed (fixed with this generator)

- A study in mesh mode read the bird's cam and rod as a clash at their
  tangency on the concave flanks, at any deflection; a cam and its
  follower are now judged there by the follower check's cuts across the
  width ([motion studies](../../docs/assemblies/README.md#motion-studies)).
- The branch watcher called the axe's drop and landing, which its lever
  loop inherits, a branch jump, and the loop solve's leftover where the
  rise starts from rest one too
  ([branch jumps](../../docs/assemblies/README.md#branch-jumps)).
- The contact table was built again at samples where the loop left the
  rod a micrometre off, half a minute a time; the follower check counted
  the fastened figure as moving with its rod (and refused it without a
  material), found a liftoff in the middle of the bird's rise from a second
  difference's noise (106 rpm), and timed the axe's fall at the top of the
  drop alone (155 ms)
  ([the follower check](../../docs/assemblies/relations.md#the-follower-check)).

And with the box (phase 3):

- With each panel fixed on its own, the cam turned on the left wall and
  each follower's loop was grounded on the top: two loop problems, the
  contact relation a row in neither, so the loop's solve moved the rod back
  from where the relation set it, a tenth of a millimetre a solve, and
  every sample after the first failed. A relation now joins its mates'
  components ([solver.md](../../docs/assemblies/solver.md#closing-loops)).
  The error said *another relation drives it too*, which none did; it now
  names another driver only when there is one
  ([relations.md](../../docs/assemblies/relations.md#when-relations-disagree)).
- Logged, not fixed: the rod's foot, ending flush with the rod's round,
  left a free edge that `check_print` reads as not watertight while
  `check_geometry` verifies the body
  ([known bugs](../../docs/known-bugs.md#a-cylinder-added-flush-with-the-tangent-of-another-leaves-a-free-edge));
  the foot reaches a nozzle's width past the rod instead.

And with the drawings (phase 5):

- A drawing could not show an exploded view: a sheet view of an assembly
  now names one of its exploded views (`exploded_view`, vNEXT;
  [exploded views](../../docs/drawings/README.md#exploded-views)).
- The bill weighed every part solid, so a printed part at 15 % infill read
  more than twice what it prints at, and nothing said which plate a part
  prints on. A printed row now carries its printed mass and its plates, in
  `bom`, the CSV and a sheet's parts list
  ([bill of materials](../../docs/assemblies/README.md#bill-of-materials)).
  The parts list wrote a pin's 0.05 g as 0.0.
- The generator's negative controls put the cams back with
  `modify_feature`, which replaces the whole spec: without the material in
  it, the cams were saved with none, and the bill could not weigh them.

`tests/contact_loop_smoke.py` builds this mechanism with this code and
holds the app to each.

## Not done here

- No undercut control. A radially sliding rounded knife cannot undercut
  on these motions: its pitch curve's convex radius at the base circle is
  the base radius plus the tip's, and on these rises it stays over the tip
  radius (CamProfile builds the bird's cam at a base of 0.5 mm, 0.5 mm of
  convex radius left). The pressure angle is what a small base breaks, and
  the second control holds that.
- The pins' flats leave play where a joint's load comes onto them: the
  rod's and the lever's pins have theirs where their link does not bear,
  but a pivot's load turns with the lever, and a printed pin is not
  measured yet. The fits are the profile's gap and press; phase 7 measures
  them.
- **The crank end is not sized for a child leaning on it.** The studies
  above say it breaks at a 9.6 kg lean and holds about 1 kg with a factor
  of 2. Sizing it for a lean is a design decision with a cascade: the
  handle's pin (Ø W / 24) and the crank's plate (W / 30) are outside the
  box, but the shaft's crank end fails too, and a thicker shaft moves the
  hubs, the axe cam's base radius (the hub's floor), the bushings and the
  box's height. Which lean to design for, and whether the shaft steps up
  outside the left wall, are for the next round.
- No lift diagram: the app draws no graph or generated table of a
  CamProfile's motion, so a cam's sheet carries the motion and the lift as
  tables the generator writes from the feature's own segments
  ([drawings, not done](../../docs/drawings/README.md#not-done)).
- The film and the print: the plan's later phases.
