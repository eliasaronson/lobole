# woodcutter

A hand-cranked automaton in a printed box: one turn of the crank is one
chop. The axe cam lifts the woodcutter's arms slowly through a rod and a
link and lets them drop onto the log; the bird's cam, on the same shaft,
makes the bird peck while the axe is rising. Everything is printed and
nothing is bought. This is the mechanism, designed and checked in the app
(phase 2 of [the plan](../../docs/plans/woodcutter-automaton.md)); the
box's panels, the crank handle and the print plates are the plan's next
phase.

```sh
tools/headless-preview.sh --start 8800
python3 generators/woodcutter/build.py --port 8800 [--width 120] [--profile "K5 PLA on P1S"] [--no-render] [--fresh]
```

Output (untracked) goes to `scratch/woodcutter/`: `woodcutter.lob`, the
receipt `woodcutter.json` (every number below, from the app) and four quick
stills: at rest, the bird pecking, the axe raised, and the raised axe from
the side. `--no-render`, or a headless instance, skips the stills.

Files: `woodcutter_spec.py` (every size and both motions, no app),
`woodcutter_parts.py` (one function a part), `build.py` (the project, the
checks, the controls, the receipt), `test.json` (the
[generator test](../README.md#the-big-generators-as-tests)).

## The mechanism

- **The box** here is the frame the mechanism needs: the floor, two end
  walls carrying the camshaft's bores, and the top with a guide boss under
  each rod, as one body open at the front and back. The pegged panels are
  phase 3.
- **The camshaft** turns on a revolute in the end walls. Each cam is a
  `CamProfile` placed on a Frame (Z the shaft, X up at rest), its own body
  with a D bore cut over the shaft's printed flat, fastened to the shaft.
- **The axe cam**: a cycloidal rise of 15 mm over 210°, the drop, and a
  150° dwell for the axe to land in. **The bird's cam**: a dwell to 25°, a
  cycloidal rise and fall of 8 mm over 85° each, a dwell. So the bird pecks
  from 25° to 195° while the axe climbs, and is done before the drop at
  210°.
- **The followers** are rods with a foot across the cam: a cylinder of the
  rod's radius, so each cam is cut for a knife of that tip radius (3 mm at
  120). Each rod slides in its guide on a slider mate and is driven by a
  `contact` relation, `toward` from gravity (negative: it falls onto its
  cam).
- **The figures.** Each rod's top carries a pin through a link, and the
  link's other end a pin on a lever: the woodcutter's arms, turning on a
  shoulder pin in the figure, or the bird, turning on a pin through its
  body in a perch. Rod, link and lever with the slider make a mate loop,
  which the loop solve closes; the figure and the perch are fastened to
  the box, and the log too. The bird's body hangs behind and a little
  below its pin, so its weight keeps its tail on the link through the whole
  peck.

### Where every size comes from

From `--width` W (the box's inside width along the shaft, 120 by default)
and the print's gap (`print_allowance` kind `gap` at the rod's size: 0.5 mm
on the built-in PLA row, the whole clearance), nothing else in
millimetres:

| size | rule | at 120 |
|---|---|---|
| walls | W / 33.3, at least four lines | 3.6 |
| inside depth | W / 2 | 60 |
| camshaft | Ø W / 15, a flat a sixth of it deep | Ø8, 1.33 |
| bores, guides, pin holes | the size plus the gap | Ø8.5, Ø6.5, Ø3.5 |
| cams | W / 10 wide, at W / 4 and 3W / 4 | 12 |
| rods and knife tips | Ø W / 20, tip its radius | Ø6, r3 |
| strokes | axe W / 8, bird W / 15 | 15, 8 |
| base radii | the least whose CamProfile reports ≤ 30° of pressure, at least the hub's (the bore and two walls) | axe 7.45 (the hub), bird 12.05 |
| shaft height | the larger cam plus W / 20 | 28.45 |
| guide bosses | three rods long | 18 |
| inside height | the highest foot at full lift, W / 40 under the boss | 74.3 |
| pins, plates | Ø W / 40; plates W / 40 thick, bodies W / 30 | Ø3, 3, 4 |
| figures | the arm W × 0.3 to the axe's eye, the axe's head W / 15 | 36, 8 |
| the blade over the log | W / 120 | 1.0 |
| the levers | each pin's arm from the stroke over the swing asked (arm −40° to 80°, bird −15° to 35°), the pivot over the rod at mid-swing | 9.2, 9.6 |

The base radii are searched against CamProfile's own report in a scratch
part (in 0.05 mm steps from the closed form), then the box is laid out
round the cams. The arm reaches 70.1° rather than the 80° asked: the link
tilts. The motions' angles are design choices: a slow rise over most of
the turn, a dwell long enough for the drop at twice the hand speed, and a
peck clear of the drop.

## What is checked every run

The run refuses to save what fails, and every number is in the receipt.
At W = 120 on the built-in PLA row:

- **Every part** is one body and `check_geometry` says verified.
- **The cams' reports**: axe base 7.45, 22.45 to the top, 26.2° worst
  pressure angle at 86°; bird base 12.05 (the closed form for a cycloidal
  rise under 30° gives 12.03), 20.05 to the top, 29.97° at 64°; both fits
  within 3e-7 mm.
- **Two motion studies**, `clash_mode: mesh` at a fifth of the gap a side
  (0.05 mm): one crank turn (49 samples, 7.5° apart) and the chop (41
  samples over 200° to 220°, half a degree apart, across the drop). Both
  clean: no clash, no branch jump, every sample solved; each cam and its
  rod `touching`.
- **`check_followers`** over both at 120 rpm, twice a hand's turn: each rod
  touches its cam across the width at every sample (the deepest entry
  3e-5 mm on the axe, 5e-7 on the bird), the realised motion is the one
  the cam was cut for (deviation 3e-5 and 1e-6 mm), gravity holds each rod
  (`held`) up to its liftoff: the axe's at 154 rpm (where the rise slows
  near its top, the arm upright), the bird's at 170 (the bare rod's closed
  form, √(gβ²/2πh), is 198: the bird takes some of gravity's hold). The
  axe's drop: 15 mm, falling in 107 ms (free fall would be 55: the arm
  carries most of the inertia and, near upright, little of the weight),
  140.4° of dwell to land in, `drop_rpm` 218.5, so at 120 rpm it lands
  after 77° of turn.
- **Requirements**, as they lie in the saved project, all passing: each
  follower's `liftoff_rpm` ≥ 120 and `pressure_angle_deg` ≤ 30, the axe's
  `drop_rpm` ≥ 120 over the chop, both studies `motion_clean`, and the
  blade's exact clearance over the log over the turn ≥ 0.9 mm (it reads
  1.000 mm, at rest).
- **Negative controls**, each of which must fail:
  - the axe's landing dwell cut to 15°: the drop lands on the next rise
    (`drop_rpm` 17, the requirement fails);
  - the bird's cam on the hub's radius, 7.45: 37.6° of pressure, the
    requirement fails;
  - a guide tilted 10° out of the cam's plane, and one moved along the
    shaft off the cam's width: each refused by `add_relation` as a contact
    (a barrel or face cam is not built; the two never meet).
  Each modifies the design and puts it back, so the last requirement check
  is on the design as saved.

Runtime: 116 s on a quiet machine (the negative controls are half of it:
each one rebuilds a cam and runs a study again), 250 s under load.

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

`tests/contact_loop_smoke.py` builds this mechanism with this code and
holds the app to each.

## Not done here

- No undercut control. A radially sliding rounded knife cannot undercut
  on these motions: its pitch curve's convex radius at the base circle is
  the base radius plus the tip's, and on these rises it stays over the tip
  radius (CamProfile builds the bird's cam at a base of 0.5 mm, 0.5 mm of
  convex radius left). The pressure angle is what a small base breaks, and
  the second control holds that.
- The box as printed panels, the crank handle, the print plates, FEM, the
  drawings and the film: the plan's later phases.
