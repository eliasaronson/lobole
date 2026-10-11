# Planetary: rolling bearers, and a check on the worst push

## The problem

The print-in-place planetary (`generators/planetary/`) has no carrier and no
axle: the sun is centred only by the three planets, through their teeth. On
the K5 PLA print (sun 14 / planets 13 / ring 40, module 1.4, 25° stub teeth,
addendum 0.8 m, helix 30°, 0.1 mm a side), a push on the sun toward the space
between two planets opened the third planet's mesh by almost a whole tooth.

A mesh's centre distance closes by g / (sin α · cos β_b) ≈ 0.27 mm before
both flanks touch. A push between two planets closes two meshes in series
(sun–planet, planet–ring) along lines 60° off the push, so the sun travels
2 · 0.27 / cos 60° ≈ 1.07 mm, and the opposite planet's mesh opens by that
plus its own play toward the ring (0.27): 1.34 of a 2.24 mm working depth,
0.9 mm (40 %) left engaged.

## The fix: rolling bearers above the teeth

Each gear gets a smooth bearer band ABOVE its teeth: the sun and the planets
a cylinder of radius pitch − g/2, the ring a bore of radius pitch + g/2 (g =
the profile's gap a side). Bearers at the pitch circles roll on each other
(the contact is at the pitch point: pure rolling, as the teeth's pitch
circles do), and a mesh's centre distance can now close by g, not 0.27 mm.
The lateral push becomes (g + g) / cos 60° = 4 g ≈ 0.4 mm, the opposite
mesh opens by 0.5 mm and about 1.74 mm (78 %) stays engaged.

**The neighbour's teeth reach under every bearer**: a band at the pitch
circle sits over the other gear's addendum. So the band stands on a 45° cone
(printable without support) that starts at the teeth's top face inside the
neighbour's tip circle and widens to the band:

- cone base radius = the neighbour's tip reach − g − (one layer + g): the
  sun's cone starts at its pitch − 0.8 m − 0.4 mm, a planet's at its pitch −
  0.8 m − 0.4 mm (the sun's and the ring's tips reach equally far into it),
  the ring's bore at its pitch + 0.8 m + 0.4 mm. Each lands at or a few
  hundredths outside its gear's root circle, so the cone overhangs nothing.
- In the first layer above the teeth, the cone stays g clear of the
  neighbour's tip in plan; above that it crosses over the neighbour's teeth
  with at least one empty layer (`LAYER_MM`, the P1S's 0.2 mm the README
  slices at) between them. A 0.1 mm normal gap is the profile's figure for
  WALLS; over a horizontal face the slicer needs a layer.
- cone height = 0.8 m + g/2 + layer + g (1.47 mm at 70 mm), band height 2 m
  (2.8 mm, 14 layers). Every gear only narrows going up: the sun's and the
  planets' bands shrink inward from their teeth, the ring's bore widens
  outward from its tooth tips.

The plate's height stays 0.3 of the diameter; the teeth give up the cone and
the band (21 → 16.7 mm at 70 mm). The knob stands on the sun's band (the hex
is inside the band), the planet numbers and the ratio are engraved in the
band tops as before.

### No bottom band

A band under the teeth would carry the teeth on its top, and their tips
(and the ring's, inward) would overhang it by an addendum — it needs the
teeth chamfered off at the bottom, which costs working face. It is not
needed: a push on the knob (above the bearers) pivots the sun about the
bearer contact, and its bottom swings the OTHER way, toward the opposite
planet. That closes the opposite planet's mesh, and is limited by its two
tooth meshes in series (2 · 0.27); the two pushed-toward planets' meshes open
at the bottom by that times cos 60° plus their own play toward the ring:
0.27 + 0.27 = 0.54 mm, 1.70 mm (76 %) engaged. The check below reports this
tilt case too and holds it to the same limit.

## The check: the worst push, measured

`push_check()` in build.py, on an assembly `push check` of free occurrences
(no mates) of the sun, planet and ring parts:

1. **Closure, measured**: translate the sun toward planet 1 along their line
   of centres until the exact B-rep distance (`measure`) reaches zero —
   `t_sp`; the same for planet 1 toward the ring — `t_pr`. The phases put
   every tooth centred in its space, so a translation closes both flanks at
   once and this is the same closure the free gears have. The search steps
   by the measured distance (moving a body by d cannot close a gap by more
   than d), so it never overshoots, then brackets.
2. **The push**, from those two numbers and the geometry of the train
   (which the model cannot show without a contact solver): the sun travels
   δ = (t_sp + t_pr) / cos(π/3), and the opposite planet's mesh opens by
   δ + t_pr (the planet drifting out to the ring). Engagement left = the
   working depth (2 · 0.8 m, the sun's and the planet's addenda) − the
   opening. The tilt case: (t_sp⁰ + t_pr⁰) · cos(π/3) + t_pr⁰ from the
   TEETH's closures.
3. **Limit: half the working depth (one addendum) stays engaged.** With at
   least an addendum engaged each tooth still reaches the other gear's pitch
   circle, so the teeth bear flank on flank; below it the contact runs out
   toward the tip corners, the gear rides up the flank instead of turning,
   and the next push skips it. The measured print had 40 % and was felt to
   be near a skip.
4. **Negative control**: the same measurement on the gears without bearers
   (parts `sun`, `planet`, `ring` built bare, as the old generator did) must
   FAIL the limit; a run where it passes stops, since the check could not
   see the fault it exists for. Those closures are also the tilt case's.

Only the push composition (step 2) is a formula; both closures, before and
after, are measured on the exact solids.

## Files

- `generators/planetary/build.py`: `solve()` gains the teeth height, band
  and cone sizes and `LAYER_MM`; a `bearer()` revolve unioned onto the sun
  and planets and cut from the ring; `herringbone()` extrudes the teeth
  height; the sun study classifies faces by the teeth height;
  `push_check()` + its control, printed and in the receipt.
- `generators/planetary/README.md`: the bearers, the push check, the numbers.
- `tests/generator_runs.py`: `generator_planetary` also waits for the push
  check's line; it stays in its 240 s budget.

## Verification

- `build.py --profile "K5 PLA on P1S" --study`: check_geometry verified,
  every meshing pair ≥ g − 0.01 (now including the bands and cones), five
  STL shells, both motion studies clean and their negative controls clash,
  the push check passes and its control fails, the sun's study solves.
- `tools/ctest -R generator_planetary -C generators` within its budget, and
  the headless suite.
