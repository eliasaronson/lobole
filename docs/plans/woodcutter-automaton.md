# Woodcutter automaton

A print-only dogfood round: a hand-cranked automaton in a printed box. The
crank turns the camshaft directly, one chop per crank turn. Two cams lift
followers that fall back by gravity, and the followers move two figures: a
woodcutter whose axe rises slowly and drops onto a log, and a bird that
pecks on its own cam, phase-shifted from the chop so the two motions take
turns. Everything is printed, the box included, on the P1S profiles. Nothing
is bought, there is no motor, and there is no CAM.

The design targets:

- **The box's inside width is about 120 mm.** It fits a P1S plate as flat
  panels and leaves room for two cams side by side on one shaft. It is the
  generator's one size argument; every other size is derived from it.
- **A comfortable hand turn is about 1 rev/s (60 crank rpm).** At one chop
  per turn that is 60 cam rpm. The followers must stay on their cams with a
  margin of two: `liftoff_rpm ≥ 120`, and every drop lands within its dwell
  at 120 rpm.
- **Followers are rods with rounded knife tips.** No roller and no pin.
  Printed rollers on printed pins are a later variant of the same
  generator, which the contact relation and the cam feature already serve.
- **The box is printed panels pegged together.**

Design and verification come first, in the app, with no print and no
measurement. Only the last phase prints and measures.

The app work this round needed is built: the `contact` relation drives a
follower from its cam's shape, the `CamProfile` feature cuts a cam from a
motion, and `check_followers` (with the `follower` requirement) says
whether a follower stays on at a speed
([relations.md](../assemblies/relations.md#contact),
[cam-profiles.md](../modeling/cam-profiles.md),
[the follower check](../assemblies/relations.md#the-follower-check)). What
follows is the automaton itself.

Two limits of that work shape the design below:

- **A follower must be rigid with its mate's moving side.** A roller that
  spins on its own pin is not a contact relation's follower; the roller
  variant either fastens the roller to its rod (its outline is the same
  turning or not) or first extends `contact::RigidGroup` to a revolute about
  the roller's own axis.
- **The exact clash check is not the contact check.** A query boolean
  between a B-spline cam and a pin in tangent contact took 175 s for one
  pair at a steep pressure angle, and booleans are fuzzy at 1 µm. Motion
  studies over cam pairs run `clash_mode: mesh`; `check_followers`' cuts
  across the width are what says the follower is on its cam.

## Phase 2: the mechanism (size: M)

`generators/woodcutter/` (`build.py`, `woodcutter_spec.py`,
`woodcutter_parts.py`, `README.md`, `test.json`). Output goes to untracked
`scratch/woodcutter/`: `woodcutter.lob`, a receipt `woodcutter.json`, and the
STLs and print job from phase 3.

- **Sizes come from one number, the box's inside width.** It is a
  generator argument. Everything else is derived from it and from the print
  rules (the profile's minimum wall, the coupon's clearances):
  - the camshaft's diameter;
  - the cams' base radii, set by the pressure-angle limit through
    CamProfile's report;
  - the follower rods and their guide bores, at the print clearance that
    `print_allowance` gives for a sliding fit;
  - the figures' scale.
- **The camshaft** turns on revolute mates in bores in the box's end walls.
  Each cam is a `CamProfile` keyed to the shaft by a printed flat:
  - the axe cam: a slow rise over most of a turn, then a `drop`;
  - the bird's cam: a cycloidal rise and fall, phase-shifted on the shaft so
    the bird pecks while the axe is rising, not as it drops. The two
    motions are two steps of one `SequenceStep` timeline, which tests the
    sequence checks on overlapping and alternating motions.
- **Followers** are printed rods with rounded feet (knife followers with a
  tip radius, so no roller and no pin to buy), sliding in guide bores in the
  box's top. Each is driven by a `contact` relation with `toward` derived
  from gravity.
- **Figures.** The woodcutter's arms turn at the shoulder on a revolute
  fixed to the figure, which is fastened to the box top. A link pinned to
  the rod's top and to the arm makes a mate loop (slider, three revolutes),
  which `AssemblyLoopSolve` closes. The log is fastened. The bird's body
  turns on a revolute at its feet and is tipped forward by its own rod the
  same way, so it pecks.
- **Checks in the receipt** (all numbers, all from the app):
  - a motion study over one crank turn (48 samples plus every drop's
    neighbourhood, mesh clash mode) is clean, and `check_followers` finds
    each follower touching its cam at every sample;
  - `check_followers`: `liftoff_rpm` per follower, the maximum pressure
    angle, `deviation_mm`, and the axe drop's fall time and `drop_rpm`;
  - the axe's head at the bottom of the drop is a stated clearance above the
    log (a `clearance` requirement over the study), so the blade meets the
    log without striking it;
  - requirements (`check_requirements`): `follower liftoff_rpm ≥ 120` for
    each follower (twice the 60 rpm hand turn), `pressure_angle_deg ≤` the
    limit, `motion_clean`, and the axe's `drop_rpm ≥ 120`.
- **Negative controls**, as the hinge box keeps them:
  - the axe cam with its drop dwell shortened below the fall time must fail
    `drop_rpm`;
  - a cam with too small a base radius must be refused for undercut;
  - a follower guide moved off the cam's axis plane must be refused.

Verified by `test.json` (the generator test harness) and by the receipt's
numbers matching the closed forms for the eccentric cam within the contact
tolerance.

## Phase 3: the box, the crank, the plates (size: M)

- **The box** is printed as panels that print flat and peg together.
  They use printed pegs, not glue or screws, sized by the coupon's press
  fit. The end walls carry the camshaft bores. The top carries the guide
  bores and is the figures' stage. Teardrop holes are used where a bore
  prints sideways (`RegenContext::printUp`).
- **The crank** is a printed handle on the camshaft's end, outside an end
  wall: one chop per crank turn needs no gear pair. The handle turns on its
  own revolute, and its radius is derived from the box (a stated fraction
  of its depth).
- **Print plates.** Every part is laid out on P1S plates by
  `export_print_job`, a few plates in all. `check_print` must pass each
  plate, stacking included. Each part has its print direction chosen so its
  loaded member has its layers across the load: the rods along their length,
  the camshaft lying down. `split_for_printing` is used where a panel is
  over the bed. The slicer's time and filament come from the installed
  slicer, not the app.
- **An assembly sequence** puts the box together in printable order
  (`check_assembly_sequence` with insertion), and the shaft goes in through
  its bores.
- Verified: `check_print` clean on every plate; the sequence check clean;
  `check_followers` and the motion study still clean with the box and crank
  in.

## Phase 4: FEM of the crank and the shaft on pins (size: S)

This phase assumes the pin support (the `wtpin` branch's plan,
fem-pin-support) has landed.

- **The camshaft** sits on two pin supports at its bores. It is loaded by the
  followers' contact forces at the worst sample, plus the handle torque that
  balances them. `check_followers` computes M, Q, q′ and q″ but does not yet
  report the force; this phase adds it to the reply (M q″ ω² + ½ M′ q′² ω² −
  Q along b, carried onto the contact normal through the pressure angle, at
  `speed_rpm`), so the loads are read, not estimated.
- **The crank handle and the camshaft's crank end** sit on pins and are loaded by a hand's push
  at the handle. The push is derived from the torque the followers need at
  the worst sample, with a stated factor for a child leaning on it.
- PLA's interlayer strength applies across layers
  (`max_principal_mpa`, `layer_normal`) for the shaft printed lying down.
  The pin support's flag on a support-edge peak must not be the headline.
- Verified: the factors of safety come out of the receipt, with the reaction
  at each pin matching the applied load's statics within 1%.

## Phase 5: drawings and BOM (size: S)

- An assembly drawing (front, side, an exploded view, balloons through
  `layout_balloons`) and `export_bom`: every printed part with its material,
  printed mass and plate. Nothing is bought, so the BOM has no fastener rows.
- Part drawings for each cam, with its lift diagram. A drawing table of the
  CamProfile's motion counts if one is there to use. Otherwise the gap is
  recorded for the roadmap.
- `check_drawing` is clean.

## Phase 6: the film (size: S)

- `export_motion_video` of the crank turning two or three times, with the
  camera on the figures. A second cut runs with the box sectioned
  (`set_section` during motion) so the cams and followers show. It is driven
  at the hand speed (60 rpm), with the sample count high enough that the drop reads
  as a drop. A `SequenceStep` film of the assembly order comes from phase 3.
- Look at the frames. The axe must meet the log on the beat, with no figure
  passing through the box.

## Phase 7: print and measure (later, when the printer is free)

Print the plates, assemble by the sequence, and measure:

- each rod's stroke against the contact table, with calipers at the dwell
  and the drop;
- the crank speed at which the axe first chatters (a phone's slow-motion
  video against a metronome) against `liftoff_rpm`. This is the friction
  the check leaves out;
- the fits: the guide bores, the shaft, and the pegs.

The numbers go into the generator's README beside the predictions. Any
difference that is a bug goes into known-bugs.md or the roadmap.
