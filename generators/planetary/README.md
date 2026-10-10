# Print-in-place planetary gear set

A sun, three planets and an internal ring with herringbone teeth, printed as
ONE plate that comes off the bed assembled and turns. The V of the teeth
holds every gear at its height, so there is no carrier, no pin and no
support. Hold the ring, turn the hex knob on the sun, and the numbered
planets orbit at the ratio engraved on the ring.

    tools/headless-preview.sh --start <port>
    python3 generators/planetary/build.py --port <port> --profile "K5 PLA on P1S"
    python3 generators/planetary/build.py --port <port> --diameter 90 --study

Output goes to `scratch/planetary/` (or `--out`): `planetary.stl` (the plate,
five shells), `planetary.dsgn`, `planetary.png` (the hero still) and
`planetary.json`, the receipt (tooth counts, module, ratio, gap, measured
clearances, motion verdicts and, with `--study`, the sun's stresses).

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
- **The module**: fifty across the ring (1.4 at 70 mm).
- **The tooth counts**: the largest ring that fits inside a wall of 7 % of
  the diameter, with `Zr = Zs + 2 Zp` (the planets fit between),
  `(Zs + Zr) % 3 == 0` (three equal planets assemble), neighbouring planets
  clearing tip to tip, at least 12 teeth on each, and the sun and planets as
  close in size as that allows. At 70 mm: 14 / 13 / 40, ratio 3.857 : 1.
- **Face width** 0.3 of the diameter, the knob 0.3 of the face width,
  engraving 1 % of the diameter deep.

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
  exact helicoid flanks about the gear's centre, a 30° helix at the pitch
  circle (the twist is height · tan 30° / pitch radius, signed so every
  gear rolls with its mates). It replaced two lofts through eight sections
  each and their union, which took 12-24 s a gear and strayed from the
  helix between sections; the extrude takes under half a second.
- **Phasing** (`phases()`): the sun's tooth 0 points along +X. Each planet
  takes the half pitch less the sun's offset at its angle, and the ring
  cutter takes the planet's tooth at the ring. The script asserts all three
  planets ask the ring for the same phase.

## What is checked, every run

- `check_geometry` on the print part is `verified`.
- **Every meshing pair's exact B-rep distance is at least the gap a side
  less 0.01 mm.** The closest points come out about 5 µm short: 0.095 mm
  (sun) and 0.097 mm (ring) against the K5 profile's 0.1. That is the
  profile, not the solid: gear.py draws each flank as two arcs through
  involute points.
- **The STL's chords are at most a fifth of the gap:** `export_stl` picks
  the profile's chord deviation (0.01 mm) for a printed part.
- **The STL has five shells**: two bodies that touch would fuse into one.
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

`--study` adds a static study of the sun, the weakest gear: a firm 60 N
pinch's torque (0.39 N m at 70 mm) as a `torque` load on the knob's six
flats about the sun's axis, held at each planet by the one flank that
drives it, in both halves of the V. Material is the profile's (PLA,
Prusament's datasheet), at 0.4 module elements. The solve goes to the GPU
(docs/simulation/README.md#the-solver).

It reports two stresses and says which is which:

- **The tooth roots:** `probe_study` on every root face, the worst max.
  At 70 mm, 2.1 MPa against PLA's 48: a hand cannot strip these teeth.
- **The global peak** (3.2 MPa) and its factor of safety. It sits in the
  sharp inside corner where the hex knob meets the gear's face, a
  re-entrant corner whose value grows as the mesh is refined, so it is a
  property of the mesh more than of the part.

The generator test `generator_planetary`
(generators/README.md#the-big-generators-as-tests) runs the default build
without the still.

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
