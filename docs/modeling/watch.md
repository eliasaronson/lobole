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
  width, with the spring-bar hole drilled through both lugs in one cut.
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
- **Perlage across the plate**, not just around its rim — a field of spots
  that keeps clear of every hole by construction.
- **Finishes that contrast.** A brushed case middle under a polished bezel,
  and a warmer dial.

The stills are four, and each names what it hides: `dial` and `case` (glass
hidden), `caseback` (through the window), and `movement` — case, caseback and
rotor off, as a watchmaker would see it.

The bridges have no pillars. They are placed by fastened mates to the
mainplate's seat, which holds them exactly, but a real bridge stands on
pillars or feet between the wheels; only the pallet bridge has them. Choosing
pillar sites clear of every wheel's sweep is what the layout gate could
check, and was not attempted.

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

**An animated escapement: no.** Its motion is intermittent, so no
constant-ratio relation expresses it, and a cam relation only drives a sliding
follower — a revolute follower is refused. The escapement is placed rather
than animated: the pallet and balance turn on revolute mates of their own, the
stones sit on the escape wheel's tip circle, and the pallet is cut clear of
the escape wheel by construction, so it is clash-free at any phase of the
train.

That refusal contradicts itself:

    mate 5 (revolute) has no offset to couple — a cam needs revolute or
    cylindrical mates

It names "revolute" as accepted in the same sentence that refuses one. The
rule it means is "the DRIVER turns and the FOLLOWER slides", and a refusal that
names the wrong vocabulary is the thing this codebase's refusal rule exists to
prevent. Not fixed here.

**Glass renders opaque.** The ray tracer hard-codes transmission to zero, so
the crystal would hide the dial in every image and the caseback window would
hide the movement. They are in the model and in its checks, and hidden for the
renders.

**There is no ruby, sapphire or blued steel.** Those are appearance
overrides on real material slugs, so the mass is the slug's.

**Meshing phases move the hands.** Every driven arbor sits up to a tooth pitch
from zero once its relation puts a gap where the driver has a tooth — which
on the 32-tooth hour wheel is 22 minutes of dial. The hands are fastened at
the angle that cancels their arbor's resting dof, read back from the solved
train, so the watch reads 12:00:00 with the barrel at zero.
