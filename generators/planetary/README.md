# Print-in-place planetary gear set

A sun, three planets and an internal ring with herringbone teeth, printed as
ONE plate that comes off the bed assembled and turns. The V of the teeth
holds every gear at its height, so there is no carrier, no pin and no
support; rolling bearers above the teeth hold the centre distances
([Bearers](#bearers)). Hold the ring, turn the hex knob on the sun, and the
numbered planets orbit at the ratio engraved on the ring.

    tools/headless-preview.sh --start <port>
    python3 generators/planetary/build.py --port <port> --profile "K5 PLA on P1S"
    python3 generators/planetary/build.py --port <port> --diameter 90 --study   # the built-in table

Output goes to `scratch/planetary/` (or `--out`): `planetary.stl` (the plate,
five shells), `planetary.dsgn`, `planetary.png` (the hero still),
`planetary.json`, the receipt (tooth counts, module, ratio, gap, helix,
measured clearances, motion verdicts, the push check and, with `--study`,
the sun's stresses), and, on a profile that states a bed,
`planetary_job.3mf`, the [print job](../../docs/tolerances/README.md#print-jobs)
(a Bambu project when `$LOBOLE_SLICER_CONFIGS` holds `pla.json`).

## Where every size comes from

The only inputs are `--diameter` (the ring's outside, 70 mm by default) and
the print process: `--profile NAME`, one of your print profiles, measured
on your printer (docs/tolerances/README.md#print-profiles), or else
`--process fdm --material pla` (or `petg`), the built-in tables, the
community's numbers. The rest derives:

- **The gap**: `print_allowance` kind `gap` for that process
  (docs/tolerances/README.md#printed-parts), half of it a side. It sets the
  backlash (the gap along the flank's normal: divided by cos of the pressure
  angle and of the base helix angle), the tip clearance (at least the gap),
  and the spacing every check below holds the model to.
- **The module**: fifty across the ring (1.4 at 70 mm), and at least
  what the gap needs for the worst push to leave half the working depth
  engaged ([Bearers](#bearers)): a push opens a mesh by about five gaps,
  so the module is at least 6.25 times the gap a side. A measured 0.1 mm
  needs 0.63; the built-in PLA table's 0.25 needs 1.6, and no set with
  12-tooth planets fits a 70 mm ring at 1.6, so without a profile the
  generator refuses below 80 mm and says why.
- **The tooth counts**: the largest ring that fits inside a wall of 7 % of
  the diameter, with `Zr = Zs + 2 Zp` (the planets fit between),
  `(Zs + Zr) % 3 == 0` (three equal planets assemble), neighbouring planets
  clearing tip to tip, at least 12 teeth on each, and the sun and planets as
  close in size as that allows. At 70 mm: 14 / 13 / 40, ratio 3.857 : 1.
- **The plate's height** 0.3 of the diameter, the knob 0.3 of that,
  engraving 1 % of the diameter deep. The teeth are what the bearers leave
  of the height: 16.7 of 21 mm at 70 mm.

## The teeth

- **25° stub teeth** (addendum 0.8 module). At 20° and a full addendum, a
  14-tooth sun's tip came out 0.29 mm wide, too thin to print from a 0.4 mm
  nozzle. At 20° the ring's tooth also had to shrink to half a module to
  clear a 13-tooth planet's radially drawn (undercut) flank. At 25° nothing
  above 9 teeth is undercut, and the stub tip is about 0.7 mm wide.
  `solve()` refuses a tip under one and a half nozzle widths.
- **The ring** is a blank cut by an external gear of the ring's tooth count
  (`involute_profile` with addendum and dedendum swapped and a negative
  backlash), its own tooth shortened only as far as the line of action's
  tangency with the planet's base circle requires.
- **Herringbone**: one sketch of the outline and one Extrude with
  `twist_deg` and `herringbone` (docs/modeling/README.md#twisted-extrudes):
  exact helicoid flanks about the gear's centre, a helix of β at the pitch
  circle (the twist is height · tan β / pitch radius, signed so every gear
  rolls with its mates). It replaced two lofts through eight sections
  each and their union, which took 12-24 s a gear and strayed from the
  helix between sections; the extrude takes under half a second.
- **The helix angle is what leaves the air.** A helical flank leans off
  vertical by the base helix angle β_b, so two meshing flanks the gap g
  apart along their normal leave g / sin β_b of vertical air between them
  where one prints over the other. At the usual 30° that is 2.2 g: 0.216 mm
  at K5's 0.1 mm gap, against the 0.4 mm the profile's air row measured
  (`z_gap_mm`), which is what stacking holds a part printed over another
  to. Of the three ways to leave more (the helix, the gap a side, a stepped
  flank), the helix is the one that costs nothing: a wider gap is more
  backlash on every mesh, and a step is a ledge the slicer must bridge.
  Under a profile that states its air, `helix_for()` takes the steepest
  whole-degree helix up to 30° that leaves it: **15° on K5** (0.424 mm
  designed, 0.4015 measured, planet 2 over the sun). A shallower helix only
  stands the flanks nearer vertical, so the 45° self-support holds; the V
  still holds every gear at its height, with that air as its axial play.
  Without a stated air the helix stays 30°. The bearers' cones likewise
  cross over the neighbour's tips by the air instead of a layer when the
  profile states more than a layer and two gaps.
- **Phasing** (`phases()`): the sun's tooth 0 points along +X. Each planet
  takes the half pitch less the sun's offset at its angle, and the ring
  cutter takes the planet's tooth at the ring. The script asserts all three
  planets ask the ring for the same phase.

## Bearers

Without a carrier the sun is centred only by the planets' teeth, and a mesh
can close by g / (sin α · cos β_b) of centre distance before both flanks
touch (0.25-0.26 mm measured at 0.1 mm a side and a 30° helix, 0.23 at
K5's 15°). A push on the sun toward the
space between two planets closes two meshes in series along lines 60° off
the push, so the sun travels about 1 mm and the third planet's mesh opens by
that plus the planet's own play: the first K5 print, pushed by hand, kept
about 1 mm of its 2.24 mm working depth.

So every gear carries a smooth **bearer band** above its teeth: the sun and
the planets a cylinder of their pitch radius less half the gap, the ring a
bore of its pitch radius plus half the gap. Neighbouring bands clear by the
gap and touch at the pitch point, where the pitch circles roll without
slipping, so the motion studies see the same pure rolling as the teeth, and
a mesh closes by the gap instead of 0.25 mm.

- **Each band stands on a 45° cone** (no support), because the neighbour's
  teeth reach an addendum under it. The cone's foot is the gap plus one
  layer plus the gap inside the neighbour's tip circle (the sun's at its
  pitch radius less 0.8 m + 0.4 mm, a planet's likewise, the ring's bore
  that much outside its pitch circle), which lands on or a few hundredths
  past each gear's root. It clears the neighbour's tips by the gap in plan
  through the first layer above the teeth and crosses over them a layer
  up: the profile's gap is between walls, and over a horizontal face the
  slicer needs an empty layer (`LAYER_MM`, the 0.2 mm the plate is sliced
  at). The cone is 0.8 m + 1.5 g + a layer tall (1.47 mm at 70 mm), the
  band 2 modules (2.8 mm, fourteen layers).
- **Every gear only narrows going up**: the sun's and planets' bands step in
  from their teeth, the ring's bore steps out from its tips. The knob stands
  on the sun's band; the planet numbers and the ratio are engraved in the
  band tops.
- **No band under the teeth.** It would carry the teeth on its top with
  their tips overhanging it by an addendum, so the teeth would have to be
  chamfered off at the bottom. It is not needed: a push on the knob, above
  the bearers, pivots the sun about them and its bottom swings the OTHER
  way, toward the opposite planet, held there by that planet's two tooth
  meshes in series. The two meshes it opens open by that times cos 60° plus
  their own play, about 0.5 mm. The push check reports this tilt case and
  holds it to the same limit.

## What is checked, every run

- `check_geometry` on the print part is `verified`.
- **`check_print`** (docs/tolerances/README.md#checking-a-print) on the
  print part, which must pass every item (stacking only under a profile that
  states its air):
  - **every pair of bodies' exact B-rep distance is at least the gap a
    side**, less the check's tenth. The closest points come out about 6 µm
    short: 0.094 mm (the sun and planet 3) against the K5 profile's 0.1.
    That is the profile, not the solid: gear.py draws each flank as two
    arcs through involute points;
  - **the STL's chords are at most a fifth of the gap**: `export_stl` picks
    the profile's chord deviation (0.01 mm) for a printed part;
  - **five pieces**: two bodies that touch would print as one;
  - **on the bed;**
  - **nothing overhangs past 45° and nothing bridges**: the helix, the
    bearers' 45° cone and the knob read 45.000 at worst off the B-rep.

  - **stacking, under a profile that states its air.** Every piece starts
    on the bed, and where the herringbone flanks mesh one gear's flank
    lies over its neighbour's. A profile whose air row has been printed and
    recorded (`z_gap_mm`, docs/tolerances/README.md#print-profiles) is a
    measured limit, and the planetary is held to it through its helix
    ([the teeth](#the-teeth)): on "K5 PLA on P1S" no face lies within its
    0.4 mm over another gear (the least is 0.4015 mm, planet 2 over the
    sun, against 0.205 at the old 30° helix). On the built-in table, or a
    profile that has not measured its air, the limit is the unmeasured
    three-layer default, and stacking is reported and not required:
    `planetary: stacking ..., reported, not required`.
  - **on the profile's bed** (`bed_fit`): the plate is built at the bed's
    centre (128, 128 on the P1S), moved along x past a keep-out zone it
    would sit on, and a plate that does not fit is refused. On the
    built-in table there is no printer, and it is built round the origin.

  The whole report is in the receipt's `print`.
- **The assembly turns.** The `sun`, `ring` and a kinematic carrier (a part
  of frames only, also placed fixed as the ground every axis hangs on) turn
  on their own axes, the planets on the carrier's frames. One `epicyclic`
  relation couples them from the tooth counts, Willis's equation plus each
  planet's turn on the carrier
  (docs/assemblies/relations.md#relations-over-several-mates). With the sun
  driven, the solve holds the ring and says so, the carrier turns
  Zs/(Zs+Zr) of the sun and each planet -(Zs/Zp) of the sun's turn relative
  to the carrier. Two motion studies
  check for clashes: one mesh cycle (every pair through every phase of its
  mesh) in 24 samples, and one whole orbit of the planets in 240. Both must
  be clean.
- **The clash check can fail at this gap.** It runs the mesh check at a
  fifth of the gap (`clash_deflection_mm`; its derived 0.2 mm was coarser
  than the 0.1 mm the teeth clear by). Before the real studies, planet 1 is
  put half a tooth out, and then only 0.4 mm along its pitch circle, and
  each must clash; a run whose controls read clean stops there.
- **The worst push leaves half the working depth engaged** (`push_check`,
  on an assembly `push check` of free occurrences). Measured: how far the
  sun travels toward planet 1 along their line of centres before the exact
  solids touch (`t_sp`), and planet 1 toward the ring (`t_pr`, against a
  45° sector of the ring around it, since an exact distance to all forty
  teeth takes seconds). The phases centre every tooth in its space, so a
  translation closes both flanks together, as the free gears' play does.
  The search steps by the measured gap, which no contact can outrun, then by
  the rate the gap closed at. Composed from those by the train's geometry:
  the sun travels (t_sp + t_pr) / cos 60°, and the opposite mesh opens by
  that plus planet 1's drift out to the ring, t_pr. What is left of the
  working depth (2 · 0.8 m) must be at least half of it, one addendum: each
  tooth then still reaches the other's pitch circle and they bear flank on
  flank; below it the contact runs out to the tip corners and the next push
  rides a tooth over. At 70 mm in K5: the sun travels 0.40 mm and 1.74 mm
  (78 %) stays engaged; at the knob's tilt, 1.78 mm (80 %).
- **The push check can fail.** The same measurement on the gears as teeth
  alone (`sun, no bearer` and the rest, what the first print was) must fail
  it, or the run stops: at K5's 15° helix 0.23 / 0.23 mm closures, the sun
  travels 0.92 mm and 1.09 mm (49 %) is left; at the 30° the first print
  had, 0.26 / 0.25, 1.01 mm and 0.98 mm (44 %) — what the hand found on it.

`--study` adds a static study of the sun, the weakest gear: a firm 60 N
pinch's torque (0.39 N m at 70 mm) as a `torque` load on the knob's six
flats about the sun's axis, held at each planet by the one flank that
drives it, in both halves of the V. Material is the profile's (PLA,
Prusament's datasheet), at 0.4 module elements. The solve goes to the GPU
(docs/simulation/README.md#the-solver).

It reports two stresses and says which is which:

- **The tooth roots:** `probe_study` on every root face, the worst max and
  the mean. At 70 mm, 6.2 MPa at worst (0.76 on average) against PLA's 48:
  a hand cannot strip these teeth.
- **The global peak** and its factor of safety, and where it is. With the
  bearer it is the root's worst too (6.2 MPa, factor of safety 7.8): the
  top of a tooth space, where the bearer's cone stands on the roots and the
  pinch's torque enters the teeth. Without the bearer it was the inside
  corner of the hex knob (3.2 MPa). Both are re-entrant corners, whose
  value grows as the mesh is refined, so a property of the mesh more than
  of the part.

The generator test `generator_planetary`
(generators/README.md#the-big-generators-as-tests) builds at 90 mm on the
built-in table (its scratch instance has no measured profile) without the
still, and waits for the push check's verdict too.

## Printing it on a P1S

1. **The gap is the printer's, kept in a print profile.** On a P1S, K5 PLA
   slides at 0.1 mm a side (0.075 rubs) and an ordinary PLA at 0.15: a set
   modelled at the community's 0.25 printed loose. Both are profiles ("K5
   PLA on P1S", "PLA on P1S"). For another printer or filament, print
   `print_coupon.py --gap-only`, push the discs out, record the tightest
   that slides with `print_coupon.py --record` into a new profile, and
   build with `--profile` it. Slice the coupon with the same preset as the
   gears; both generators export STL at the profile's fine chords, since at
   0.1 mm the facets took 0.03 mm a side off every gap and the slicer's gap
   closing shut the narrowest.
2. Slice `planetary.stl` flat on the bed as it comes, **no supports**, 0.2 mm
   layers.
   - **No brim**, or an outer brim only. A brim between the gears welds them.
   - The first layer is where a print-in-place part fuses, so keep elephant
     foot compensation on.
3. Off the bed, hold the ring and turn the knob firmly once to break any
   first-layer whiskers. Then it should spin.
