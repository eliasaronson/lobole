# An automatic watch — what building it found

`generators/watch/` builds a 21,600 vph self-winding watch with small seconds
in a 40 mm case (the plan: [automatic-watch-plan.md](automatic-watch-plan.md)).
It is the first model built on the receipts, declared-clearance and `Param`
machinery from the start rather than converted onto it. This is what building
it found — in the design, in the tools, and in the app.

## The arithmetic caught four errors before any geometry existed

Every number in the movement was checked by script before it was written
down, and the checks now run at import in `watch_spec.py`, so no document is
ever built from a layout that cannot run.

1. **A sign error placed the third wheel wrongly.** The first formula for the
   point at r₁ from the centre and r₂ from the fourth arbor gave y = −3.604
   where the answer is −3.896. A distance assertion catches it; eyeballing a
   plan does not.
2. **The obvious fourth wheel sweeps through the third arbor.** 84/7 gives the
   12:1 the escapement needs, but an 84-tooth wheel has a 4.30 mm tip radius
   and sits 4.25 from the third arbor. 72/6 gives the same ratio with a 3.70
   tip. The check that caught it — no wheel may pass over any arbor except the
   one whose pinion it drives — compares every wheel against every arbor, and
   the first version only compared wheels with wheels, which passed.
3. **A plausible motion works cannot exist.** Cannon 12 → minute wheel 36,
   minute pinion 10 → hour wheel 40 is 12:1, but both pairs share the same two
   axes, so their tooth SUMS must be equal: 48 against 50. 10→30 / 8→32 is
   40/40.
4. **The pallet arbor went through the balance.** The layout check refused
   the balance rim sweeping over the pallet arbor 2.67 away — correctly: a real
   pallet arbor is short, pivoting in a pallet bridge that sits UNDER the
   balance. Giving each arbor its own depth span, and the pallet bridge its
   level, is what a watch does; the check was right and the model was wrong.

And one in the spec itself: `BARREL_OUTER_R = 4.70` was a transcribed number,
in a file whose docstring says numbers are derived, never transcribed. A
96-tooth barrel at module 0.1 has a 4.90 tip — the layout check had been
checking the wrong barrel. It is derived now.

## Involute teeth cannot run a watch train — the gates proved why

This is the largest finding, and nobody set out to look for it.

The first build cut every gear with `generators/gear.py`'s involute. Placed and
phased, every mesh rested 3 µm clear — the relation phases were right. Driven
through one pitch of the escape pinion, the fourth/escape pair INTERFERED:
0.000005 mm³ in exact booleans, symmetric about mid-pitch. Not a tessellation
artefact; a boolean.

The mesh gate then showed it was not one mesh. Unshifted, eight of the nine
interfere: below ~17 teeth the wheel's tip reaches past the interference point
— the tangent point of the line of action on the pinion's base circle — into a
flank `gear.py` draws radial rather than undercut. The two involute remedies
both fail on the escape pinion:

| remedy | fourth/escape |
|---|---|
| top the wheel to the interference limit | contact ratio 0.92 — one tooth pair leaves before the next arrives |
| profile-shift the pinion | interferes below x 0.53, POINTED above x 0.42 — no shift is both |

The gate first PASSED the shift at x 0.7 — because it checked interference and
contact ratio and never asked whether the tooth still had a tip. `gear.py`,
given a pointedness guard to try it, refused to draw it. So the gate learned to
ask, and then refused the 8-, 10- and 11-leaf pinions at x 0.5 as well.

That is why horology cuts cycloidal teeth: each gear's addendum an epicycloid,
the mate's flank the hypocycloid of the same rolling circle — at half the
pinion's pitch radius, a straight radial leaf. `watch_gearing.py` does that,
at the exact pitch centre distance with the backlash in the teeth. The same
72/6 pair, swept exactly through a pinion pitch in 25 steps: **no
interference, its flanks 12.95–16.55 µm apart the whole way**, contact ratio
1.36. All nine meshes gate at 1.36–2.15.

`gear.py` was briefly given a profile-shift parameter to try the involute fix,
and reverted when the watch stopped needing it.

**The last two interferences were not the teeth.** With cycloidal teeth the
going train swept clean, and exactly two meshes still interfered — at REST,
0.00033 and 0.00035 mm³: cannon pinion / minute wheel and rotor pinion /
reduction wheel. Rest interference with correct phases means something other
than the tooth form, and it was: both pinions rode on a core of radius 0.40 —
the cannon pinion's tube, the rotor's staff — around a pinion ROOT of 0.34. The
core filled the gaps between the leaves, and the wheel's tips, reaching 0.37
from the axis, struck the tube. The going-train pinions ride 0.15 staffs,
which is why they were clean. The mesh gate had assumed every staff is
`ARBOR_R`; it now reads each tooth set's actual core (`CORE_R`) and refuses
the old 0.40 rotor staff by name — the tube now starts above its pinion and
the rotor staff is 0.25 with its hub boss above the pinion's level.

## Making it look like a watch

The first stills were of a watch that kept time and looked like a drawing of
one: box lugs, a flat ring for a bezel, a rotor over a bare plate, and pivot
holes that were only holes. Everything that changed it is ordinary modelling,
and none of it touched a gate:

- **Lugs drawn in side view.** A profile on a YZ plane — the top falling
  from under the bezel in an arc to a rounded tip — extruded to the lug's
  width, with the spring-bar hole drilled through both lugs in one cut, and a
  half-millimetre taper in plan. The first version hung each lug on the case
  by a CORNER: its root started at a literal R − 3, which is inside the wall
  at the lug's inner face (x 10, wall at 17.3) and 1.3 mm outside it at the
  outer face (x 12.4, wall at 15.7). The root is derived from the outer face
  now, where the wall is nearest the axis, and `check_case()` refuses one
  that would reach into the dial or movement seat. Nothing had caught it: a
  lug fused along a 0.3 mm sliver is still one valid solid.
- **A turned bezel.** Its section revolved: a rounded outer edge and a
  polished bevel down to the crystal.
- **Jewels.** A ruby in every bridge pivot, each its own body so it can
  carry the ruby look, bored to the hole's radius so the staff through it
  still touches nothing — the exact sweep and the declared clearances
  re-verified unchanged.
- **Blued screws let in flush.** A proud head would need clearing from the
  rotor and from the reduction wheel that runs over the barrel bridge; a
  flush one needs nothing. The barrel bridge's two are placed by search — the
  points on a circle farthest from its three holes and from each other.
- **A mainspring.** The barrel is hollow now — a 0.30 wall, a floor behind
  the tooth ring and a 0.20 cover — and a steel strip coils inside it from
  its hook clear of the arbor out to the drum wall, as it lies let down: two
  B-spline spirals a strip's thickness apart, closed at both ends, extruded
  to the chamber's height. No still shows it; a section view does, and a
  section through a barrel is where "is there a spring in it?" is asked.
- **Perlage across the plate**, not just around its rim — a field of spots
  that keeps clear of every hole by construction.
- **Finishes that contrast.** A brushed case middle under a polished bezel,
  and a warmer dial.

The stills are five, and each names what it hides: `dial` and `case` (through the
crystal), `caseback` (through the window), `movement` — case, caseback and
rotor off, as a watchmaker would see it — and `keyless`, the dial side with
the dial and hands off.

**The bridges stand on pillars.** They first floated 3.6 mm over the plate,
held only by a mate — the same fault as the unconnected crown. A pillar runs
the whole height every wheel turns in, so it must clear all of them whatever
their depth; `watch_spec.pillars()` scans the rim for points clear of every
wheel, the balance, the pallet fork's swing and every arbor, and gives each
bridge the nearest (the cock one, the train and barrel bridges two, spread
apart). Each bridge's outline grows to reach its pillars, and each blued
screw sits on one, as a bridge screw runs down through its pillar into the
plate. Two things it moved: the balance cock's foot, which had stood at the
rim straight out from the balance — over the rim's own sweep, where a pillar
would have gone through it — and the barrel bridge, whose disc sat wholly
over the barrel and had nowhere to stand until it grew past it.

## The keyless works — the crown was connected to nothing

The first build placed the stem and crown on a revolute of their own, 1.8 mm
behind a 1.2 mm plate, with a toothless disc for a winding pinion. They are a
real keyless works now (the plan's "Keyless works" has the layout): the crown
winds the ratchet crown-in, and sets the hands crown-out without moving the
train.

**Everything is on the stem's line, and the crown wheel moved to get there.**
A pinion on a stem meets a flat wheel at a right angle, and the wheel's axis
must cross the stem's line — beyond the stem's end, or on the side its arbor
does not run to. So the setting wheel (meshing the minute wheel) and the
winding wheel both stand at y = 0, and the crown wheel went from 24 teeth
beside the ratchet to 70 teeth at (7.678, 0): the nearest point on that line
from which an arbor can pass the barrel (5.90 from its axis, against a 4.93
tip) and still mesh the ratchet.

**Contrate teeth are cycloidal racks — and a right-angle pair is only
conjugate at one radius.** Past the contact a contrate tooth moves as a rack
tooth, so it is drawn as the cycloidal rack conjugate to its wheel
(`watch_gearing.rack_profile`). But it moves at ω·r, and r varies across the
wheel's thickness, while the wheel moves at one speed: the true pitch
surfaces of a right-angle pair are cones, and a flat wheel is not one. With
0.20 mm wheels the mismatch was ±20% and the mesh interfered 0.00018 mm³.
Real keyless works run on backlash over a narrow face, and so does this one:
0.10 mm wheels (±10%) and contrate teeth at 0.25 of the pitch sweep clean at
17 samples through a pitch, crown in and crown out.

**The cannon pinion slips, because its friction fit is a relation.** Crown
out, the setting wheel turns the minute wheel, and the cannon pinion must
turn on the centre arbor without turning the train. It has its own revolute,
and a 1:1 relation to the centre arbor IS the friction fit; the setting
position removes it.

**Three things about relations the keyless works found** — all three fixed in
the app since ([../watch-app-fixes-plan.md](../watch-app-fixes-plan.md)), and
the generator now uses the fixes; what it did before is kept here because it
is what the defects cost:

1. *A mate takes one relation.* The solver settles a mate when the first
   relation reaches it and silently skips any later one. The clutch was a
   cylindrical mate turned by the stem and slid by the yoke; the yoke's
   relation was accepted by `add_relation` and never applied — the clutch
   stayed put. That is a relation the document holds and the solve ignores,
   which is what this codebase's refusal rule exists to prevent. The
   generator made the clutch a revolute whose axial seat a script moved.
   *Fixed:* the solve settles per (mate, DOF), and a relation it still
   cannot satisfy is reported on the relation — the clutch is cylindrical
   again, slid by the yoke's relation.
2. *A relation is applied only when something drives it.* Built, the stem's
   side of the watch sat at the dofs its arbors were placed with — off their
   relations' phases — until a mate on it was driven. The winding check read
   its "before" from that state and got 53.378° where the chain gives
   82.031°. `build_assembly` drove the stem once, after adding them.
   *Fixed:* a plain solve now applies every chain end to end, and the build
   no longer drives anything to make its relations hold.
3. *A configuration cannot hold "crown out".* An assembly configuration
   suppresses occurrences only — not a mate, not a relation, not a mate's
   offset — and a relation touching a suppressed occurrence still drives it.
   So the generator made the setting position by script (pull the stem, swap
   the clutch's relation, let the cannon pinion slip) and put the winding
   position back. *Fixed:* a configuration can now suppress mates and
   relations and hold mate DOFs (format v101). The watch has two: "Crown in"
   takes out the clutch's relation to the setting wheel and holds the stem
   pushed in; "Crown out" takes out the dogs and the friction fit and holds
   the stem pulled, and the yoke's relations slide the clutch by themselves.
   Both HOLD the stem, and that is what says which end of the lever chain is
   the input: unheld, a plain solve roots a chain at its first relation's
   driving side — the setting lever, since a rack runs from the turning part
   to the sliding one — and crown in first came back with the lever at its
   crown-out angle, the stem pulled and the clutch still on the setting
   wheel (0.0027 mm³ in the sweep). The document is saved
   crown in. The base — every relation in — over-constrains the keyless
   works by construction (the setting wheel driven by the clutch AND the
   minute wheel), and the solve says so if it is activated: an authoring
   state, not a position of the crown.

**Winding by hand turns the rotor backwards**, 4 turns per quarter turn of
the crown: the rotor's reduction train is related to the ratchet, and a
relation has no one-way form. A real watch has a click and a reverser for
exactly this. Left out, and said here.

## What the app could and could not do

**A wheel and its pinion on one part: yes, by composition.** `gear.py` makes
one gear per part and cannot stack two, but a tooth outline is only sketch
entities, so `watch_parts.arbor()` sketches the wheel at one depth, the pinion
at another, and a staff through both — first with `gear.involute_profile()`,
now with `watch_gearing.profile()`. A cycloidal 72-tooth wheel is 720 entities
and a 6-leaf pinion 60; the pair builds and checks `verified`.

**A hairspring: yes, without a spiral primitive.** A fit spline through an
Archimedean spiral, piped in round wire: twelve turns, 240 points, Ø0.05 mm,
about a second, and a volume within 0.4% of wire area × spiral length.

**An animated escapement: yes, since v102.** A cam relation's follower may
turn now ([rotating-cam-follower-plan.md](../rotating-cam-follower-plan.md)),
and two cams off the escape wheel drive it. One full oscillation of the
balance lets one tooth pass, so the balance swings once per tooth pitch —
the escape wheel turns 12 times a minute and has 15 teeth, which is 3 Hz or
21,600 beats an hour, the standard rate. Its table is harmonic, 270 degrees
either way, which is how a balance moves in time. The fork's table is the
impulse pin's own geometry: while the pin is in the notch the fork points at
it, and once the pin has left (47 degrees of balance either side) the fork
rests at its banking angle, 6.7 degrees. The table is sampled twenty-one
times across each passage, where the fork turns 13 degrees in 1.5 degrees of
the wheel, so the spline keeps the pin inside the notch's 0.04 mm of play.
An "Impulse" motion study drives the escape wheel through one passage a
tenth of a degree at a time and checks every pose for clashes.

Getting the swing down to a real one moved the pin. At 0.55 mm from the
balance staff the fork swung 13.5 degrees either way and its stones would
have run 0.37 mm into the wheel; at 0.30, with the horns shortened to end
just past the pin, it swings 6.7. The swing found one more thing the placed
fork never met: the pallet bridge's feet come down through the fork's level,
straight across its arbor 1.0 mm out, and stood 0.02 mm from the arms at rest.
The exact sweep caught the swung fork in one (0.0007 mm³); they stand in the
gap between the fork's body and its arms now, 70 degrees either side of its
line, 0.40 mm clear at either banking.

**What is still approximate:** the escape wheel turns continuously, not in
steps, so no stone can lock or impulse a tooth. The stones sit where the
deeper one is 0.05 mm off the tip circle at banking, and the fork is cut
clear of the wheel across its whole swing.

Before v102 the escapement was placed rather than animated, because a cam
could only drive a sliding follower and refused a revolute one. That refusal contradicted itself:

    mate 5 (revolute) has no offset to couple — a cam needs revolute or
    cylindrical mates

It named "revolute" as accepted in the same sentence that refused one,
because it picked the word from the DRIVER's dof. It now names what the
refused side needs — "b of a cam needs a slider or cylindrical mate — a cam's
follower slides" — and `tests/assembly_relations_smoke.py` pins it.

**Glass is see-through now.** The ray tracer hard-coded transmission to
zero, so the crystal hid the dial in every image and the caseback window hid
the movement, and both were hidden for the renders. Glass refracts in the
still now ([glass-plan.md](../rendering/glass-plan.md)) and the stills keep
both. Like an uncoated crystal it veils the dial a little — the minute track
fades under the reflection; a real one is anti-reflection coated, which is
not modelled.

**Ruby is a material now; blued steel is not, on purpose.** The jewels, the
pallet stones and the impulse pin were a red appearance on NO material — and
a body without one reports no mass at all. `ruby` (synthetic
corundum, 3.98 g/cm³, 345 GPa) is its own row in the material library. Blued
steel stays `steel` with a blue appearance: the blue is an oxide film a few
hundred nanometres thick, so the steel's properties are the honest ones.

**Meshing phases move the hands.** Every driven arbor sits up to a tooth pitch
from zero once its relation puts a gap where the driver has a tooth — which
on the 32-tooth hour wheel is 22 minutes of dial. The hands are fastened at
the angle that cancels their arbor's resting dof, read back from the solved
train, so the watch reads 12:00:00 with the barrel at zero.
