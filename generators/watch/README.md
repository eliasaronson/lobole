# Automatic watch

A self-winding mechanical watch, time only with small seconds: a 21,600 vph
(3 Hz) movement in a Ø40 steel case with a display caseback. Barrel, centre,
third, fourth and escape wheels; a Swiss lever escapement that swings, with a
hairspring; motion works under the dial; a rotor winding the barrel through a
reversing, one-way reduction train; a click holding the ratchet; and a keyless
works, so the crown winds the mainspring and, pulled out, sets the hands.
Dial with applied indices and a sunk small-seconds register, blued hands, a
ruby in every bridge pivot, perlage and Côtes de Genève.

## Running it

```sh
python3 generators/watch/watch.py [--port N] [--out DIR] [--stage parts|assembly|all] [--recheck]
```

`--out` defaults to `scratch/watch/`. `parts` builds and checks the parts;
`assembly` adds the assembly and every check and saves `watch.dsgn`; `all`
also renders five stills (`dial`, `case`, `caseback`, `movement` with case,
caseback and rotor off, `keyless` with dial and hands off). The run refuses
to save what it has not checked, and gates on each check's RECEIPT, not on
its `ok`. The saved document is crown in, at 10:08:37.

Most of a full run is the per-mesh exact sweep. A CLEAN result is kept in
`<out>/mesh-cache.json`, keyed on the generator's four files and the app's
source (git HEAD plus any uncommitted diff under `src/`); a run where neither
changed skips the sweep, and an instance whose binary was rebuilt under it
never uses the cache. `--recheck` forces the sweep.

`tests/watch_smoke.py` (headless, in the suite) builds the parts and the
assembly and checks the train keeps time exactly, the crown winds and sets,
nothing is placed by a typed transform, and the declared clearances hold. The
generator's own run does the expensive half: the exact sweeps, the motion
studies, the renders.

## Structure

| file | what |
|---|---|
| `watch_spec.py` | every dimension (CLASS, MOVEMENT, CASE), the layout derived from it, and the checks that run at IMPORT; `declare(app)` binds every dimension as a document variable |
| [`../cycloidal.py`](../cycloidal.py) | cycloidal tooth outlines as sketch entities, tooth 0 along +X; `rack_profile` for contrate teeth. |
| `watch_parts.py` | one function per part; `arbor()` puts a wheel and its pinion at two depths on one staff |
| `watch.py` | parts, assembly, mates, relations, configurations, declared clearances, verification, renders |

Coordinates: Z up, the watch dial-up, movement centre at the origin, 3
o'clock = +X; Z = 0 is the mainplate's dial face, and bridge-side depths are
measured down from its bridge face. Tooth counts are integers in the spec and
stay out of the document (a count is not a dimension).

## The movement

Module 0.10 throughout. Wheels meet at the exact pitch centre distance
m(z₁+z₂)/2, with the backlash in the teeth (a wheel tooth takes 0.47 of the
pitch, a pinion leaf 0.42).

| arbor | wheel | pinion |
|---|---|---|
| barrel | 96 | — |
| centre (1 turn/h) | 80 | 12 |
| third | 75 | 10 |
| fourth (1 turn/min, small seconds at 6) | 72 | 10 |
| escape | 15 club teeth | 6 |

Centre to fourth is 80/10 × 75/10 = 60; fourth to escape 72/6 = 12, so the
escape wheel turns in 5 s and its 15 teeth give 21,600 beats an hour. The
fourth wheel is 72/6 rather than 84/7 (also 12:1) because an 84-tooth wheel
would sweep through the third arbor. Motion works: cannon 10 → minute wheel 30,
minute pinion 8 → hour wheel 32 = 12:1; both pairs share two axes, so their
tooth SUMS must be equal (40 and 40).

**The teeth are cycloidal**, not `gear.py`'s involute: at 6–12 pinion leaves
an involute train interferes below ~17 teeth, topping the wheels drops the
escape mesh's contact ratio under 1, and no profile shift clears the 6-leaf
pinion without making it pointed. Each addendum is an epicycloid and the
mate's flank the hypocycloid of the same rolling circle (at half the pinion's
pitch radius, a straight radial leaf). Where one tooth set meets two mates
(the ratchet, the setting wheel), one rolling circle serves both.

**Checked at import** (`watch_spec.py`): no wheel passes over any arbor but
the one whose pinion it drives, no two wheels overlapping in plan share a
level, every mesh keeps a contact ratio of at least 1.1
(`MIN_CONTACT_RATIO`), no tip reaches its mate's actual staff (`CORE_R`), and
no lug root reaches into the dial or movement seat (`check_case`). A layout
that fails any of these cannot build a document.

Wheels overlapping in plan run at staggered depths; the pallet arbor is short
and pivots in a pallet bridge under the balance, which is how the balance rim
passes over it. Every bridge stands on pillars sited by `watch_spec.pillars()`
where nothing turns between plate and bridge, and grows out to reach them.

## The assembly

The mainplate is fixed and every other occurrence is placed by a mate —
revolutes for the arbors, cylindrical mates for the stem and clutch, fastened
mates for bridges, dial and case — so nothing holds a typed transform. Gear
relations couple the going train and motion works, so one
motion study turns every hand at its true rate. Each hand is fastened at the
angle that cancels its arbor's resting phase, so the watch reads 12:00:00
with the barrel at zero. The clearances that decide whether it runs (hands to
dial, to each other, to crystal; rotor to ratchet and caseback) are DECLARED
on the assembly and verified there.

**Escapement.** Two CAM relations off the escape wheel drive the balance and
the fork, each a turning follower: the balance swings harmonically, once per
tooth pitch; the fork's table is the impulse pin's own geometry — pointing at
the pin while it is in the notch, at its banking angle once it has left. The
escape wheel turns continuously, so no stone locks or impulses a tooth; the
stones stand clear of its tip circle and the fork is cut clear of it across
its whole swing. The hairspring is a fit-spline Archimedean spiral piped in
round wire; the mainspring is a let-down coil in a hollow barrel.

### Keyless works and winding

Everything that rides on the stem, and both wheels it drives, lie on the
stem's line (y = 0), because a pinion on a stem meets a flat wheel at a right
angle and the wheel's axis must cross the stem's line. The crown wheel's
arbor stands at (7.678, 0), the nearest point on that line from which it can
pass the barrel and still mesh the ratchet.

- **Contrate teeth** are the cycloidal rack conjugate to their wheel. A
  right-angle pair is conjugate at only one radius, so the keyless wheels are
  thin (0.10 mm) and run on backlash, as real ones do.
- **The cannon pinion slips.** It has its own revolute, and a 1:1 relation to
  the centre arbor IS its friction fit; crown out, that relation is out.
- **The clutch** is one cylindrical mate, turned by the stem and slid by a
  relation from the yoke. Its dogs are a FREEWHEEL with `teeth` 4 and a
  `ride` that lifts the clutch along the stem while it slips: the whole lift
  off a notch and none while seated, because square dogs part way over each
  other would be inside each other. The ride is ADDED to what the yoke gave
  that DOF, and the solver's residual check compares against base plus ride.
  So the rotor winding leaves the crown still, and the crown turned back rides
  the dogs over instead of unwinding the barrel.
- **Two positions are two configurations.** "Crown in" suppresses the
  clutch's relation to the setting wheel and holds the stem pushed in;
  "Crown out" suppresses the dogs and the friction fit and holds the stem
  pulled — the yoke's relations slide the clutch onto the setting wheel by
  themselves. Holding the stem is what makes it the input of the lever chain.
  The base configuration, with every relation in, over-constrains the keyless
  works and says so.
- **One-way reduction and reverser.** The wheel the rotor pinion drives rides
  loose on the staff of the pinion that drives the ratchet, joined by a
  FREEWHEEL; a second wheel meshes the first wheel to wheel, so it turns the
  other way, and rides its own pinion behind its own freewheel. Both pinions
  mesh the ratchet, so the rotor winds whichever way it swings, and the crown
  winds with both wheels still.
- **The click** is a CLICK relation on the barrel arbor: it may turn the
  winding way and is held the other. The pawl on the barrel bridge is real
  geometry, rocked over each of the 48 ratchet teeth by a cam whose table is
  computed from the ratchet's flank and the pawl's whole outline.

## What a run checks

The gear ratios over one minute of the barrel; every mesh exactly through one
pitch of its smaller gear (the whole-assembly study cannot see the slow
meshes' interference); an exact interference sweep at rest, on its receipt;
winding crown in, the rotor winding both ways with the crown still, the dogs
riding over, and setting crown out with the train still and the setting pose
swept; the declared clearances; a motion study through one escape pitch; and
an "Impulse" study through the pin's passage.

## Approximate, and not modelled

- The dogs are square and lift all at once; a real Breguet tooth ramps, which
  needs a swept helical tooth.
- No reverser spring; the mainspring pulls on nothing, so the click holds
  against a torque that is not modelled.
- The escapement is animated, not simulated: no lock, no impulse.
- Blued steel is `steel` with a blue appearance; jewels are the `ruby`
  material. The crystal is uncoated glass.
- The movement is about 8 mm thick; a thinner one means re-stacking every
  depth.
