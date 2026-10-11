# Print-in-place counter, and the intermittent relation it needs

A four-digit mechanical counter printed as one piece on the P1S: number
wheels on one axle inside a housing with a window per digit, a knob on the
units wheel, and a carry that steps the next wheel one digit per turn and
locks it in between. It comes off the bed reading 0000 and counts.

## The problem it exposes

Every relation today is continuous: gear, chain, linear, epicyclic, even a
cam's lift table repeats every turn and returns to where it started. A carry
is none of these. Per turn of a wheel the next one advances ONE step during a
window of the input's rotation and stands still the rest of the turn, and
the count accumulates over every turn. A cam cannot say it (it is periodic),
a gear cannot (it is continuous), a freewheel cannot (it never stops when its
input moves forward). So the assembly cannot be made to count, and no motion
study can check a carry.

## The app: an `intermittent` relation

One new relation kind, rotation (a) → rotation or slide (b):

    x = (a − start) / period,  n = ⌊x⌋,  u = (x − n) · period
    b = phase + step · (n + s(u / duration))   for u < duration
    b = phase + step · (n + 1)                 otherwise

A pure function of a, with no history: a wheel turned back runs the count
back, and a pose at 999 turns is as cheap as one at zero. `s` is the motion
law over the window, 0 → 1:

- `uniform`: s = t, a gear-driven step (a two-tooth carry).
- `geneva`: the exact law of an external Geneva with `slots` N. The pin at
  radius R = C·sin(π/N) enters the slot radially; with α the driver's angle
  from the line of centres, the wheel turns β = atan2(R sin α, C − R cos α),
  so s = (β + π/N) / (2π/N). The window is 180 − 360/N degrees and the step
  −360/N (an external Geneva counter-rotates, as a gear pair does); both are
  derived from `slots`, and giving them as well is refused.

`add_relation {type: "intermittent", a, b, start_deg, motion?: uniform |
geneva, step_deg | step_mm, duration_deg, period_deg?}` or `{…, motion:
"geneva", slots}`. `start_deg` (where in a's turn the window opens) is
required: the timing is the whole of a carry and a default would be a
plausible wrong one. `period_deg` defaults to one turn. `phase` defaults to
where b stands, so adding one moves nothing. An unknown motion is refused
naming the two; a step in the follower's wrong unit is refused, as a cam's
rise is; a geneva follower must turn.

It is not invertible (a locked output says nothing about where the input
is), so it propagates forward only, as a cam does. Driving b backwards
leaves a where it is and the relation reports `not satisfied` — which is what
a locked wheel is. In a mate loop it is a residual row like a cam's.

Stored as `step` (follower's unit, degrees for a turn), `period_deg`,
`start_deg`, `duration_deg`, `motion`, `slots`: kVersion 173, golden
`assembly/v173-intermittent-relation`, a row in format.md.

Files: `src/core/Occurrence.h` (the kind, its fields, `IntermittentValue`),
`src/core/AssemblySolve.cpp` (value, validation, forward-only),
`src/core/AssemblyLoopSolve.{h,cpp}` (the row), `src/core/io/SerializeAssembly.cpp`,
`src/remote/RemoteAssembly.cpp` (add/set/list and their describe rows),
`tools/designer_sdk.py` (`Asm.intermittent`), `src/ui/OccurrencePanel.cpp`
(the row's label, and Intermittent in Add relation), `docs/assemblies/relations.md`,
`docs/versioning/format.md`.

## The mechanism: a three-slot Geneva per carry

The classic odometer carry is a two-tooth segment and a lock ring on each
wheel driving a pinion whose teeth are alternately full and half width. At
this size it is wrong for the printer:

- its lock is two tooth TIPS resting on a ring; a six-tooth pinion at the
  module that fits a digit wheel has tips under 1.5 nozzle widths, which the
  0.1 mm gap then thins further;
- its motion during the carry is a gear mesh made of two teeth, with no law
  the relation can state exactly, so a clash check at a fifth of the gap
  would be checking a guess.

A Geneva's lock is a concave arc riding a lock disc over a long arc, its
pin is a stout cylinder, it enters and leaves the slot at zero velocity, and
its law is closed-form, so the relation IS the geometry and the clash check
at gap/5 means something. Three slots, not four: the window is 60° of the
driver instead of 90°. A carry centred between the units' 9 and 0 then
leaves the next wheel 1° (a 36th of a digit) off its digit at those two rest
positions; four slots leave it 4.5°, and the offsets cascade to 8° at 0000.

Each wheel j carries a pin and a lock disc; a transfer on a parallel axis
carries the Geneva (steps 120°) and a 9-tooth pinion turning the next wheel's
30-tooth gear: 120° · 9/30 = 36°, one digit, the same way round.

## Geometry (generators/counter/)

Everything derives from `--digit-height` (8 mm), `--digits` (4) and the
profile's gap, plus the 0.2 mm layer the profile was measured at:

- digit cell = height / 0.7 around the band, so band radius = 10 cells / 2π;
- transfer axis C = band radius + gap + its post's radius (the post is the
  housing's, rising past the band); Geneva R = C·sin 60°, wheel radius C/2;
- gear module from C and 30 + 9 teeth, 25° stub; `solve()` refuses a pinion
  tip under 1.5 nozzle widths, a Geneva horn under 4;
- lock disc as large as the horns allow, relieved by the Geneva's circle at
  mid-window.

**Print orientation**: axle vertical, units on top with the knob, most
significant digit on the bed. Every surface that slides is a vertical wall,
spaced by the profile's XY gap. Where a part sits over another (wheel on
wheel, Geneva on band, housing fin on band) the space is one layer, the
slicer's support-interface distance: each part is printed on the one below
as on a support, and freed with a firm first turn. No unsupported overhang
wider than a nozzle except the band's underside, chamfered at 45°. Order per
wheel, bottom to top: lock disc and pin, band (digits), 30-tooth gear. The
housing is a block minus every moving part's swept envelope (plus the gap
and the layer), so it also forms the fins that keep each wheel and transfer
at its height, the posts, a window per digit and the knob's hole.

Parts: one per wheel, `transfer`, `housing`; an assembly with a revolute per
wheel and transfer to the housing's frames, `intermittent` (geneva, 3)
wheel → transfer and `gear` (9:30) transfer → next wheel; a print part made
of `Derived` copies at the print pose (reading 0000).

## Verification

Every run (the generator refuses to save what it has not checked):

- `check_geometry` verified on the print part; every running pair's exact
  B-rep distance ≥ gap a side − 0.01; STL at the profile's chords, one shell
  per body.
- Motion studies, mesh clash at gap/5, must be clean: the units through ten
  digits and a carry (0000 → 0011), and the triple carry 0998 → 1001.
- Readable: at every rest sample of those studies each wheel stands within
  1.5° of a digit, read from the studies' own poses.
- Locked: for each carry, the next wheel turned 3° either way with the units
  at rest must clash (its Geneva's arc into the lock disc).
- Negative controls that must fail: a transfer's Geneva phased 10° wrong
  clashes over the carry; a transfer's gear half a tooth out clashes; a
  transfer built at half the gap fails the clearance gate.
- `generator_counter` in the generators ctest configuration, like
  `generator_planetary`.

App: `tests/test_assembly_relations.cpp` (the value at window edges, the
Geneva law against the pin's geometry, many turns, forward-only, locked
output reported, a loop row), serialization round trip and golden,
`tests/remote_smoke.py`'s kinds, `refused_calls`-style refusals in the
assembly relations smoke, and a GUI test that adds one from the panel.
