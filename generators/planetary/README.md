# Print-in-place planetary gear set

A sun, three planets and an internal ring with herringbone teeth, printed as
ONE plate that comes off the bed assembled and turns. The V of the teeth
holds every gear at its height, so there is no carrier, no pin and no
support. Hold the ring, turn the hex knob on the sun, and the numbered
planets orbit at the ratio engraved on the ring.

    tools/headless-preview.sh --start <port>
    python3 generators/planetary/build.py --port <port>
    python3 generators/planetary/build.py --port <port> --diameter 90 --study

Output goes to `scratch/planetary/` (or `--out`): `planetary.stl` (the plate,
five shells), `planetary.dsgn`, `planetary.png` (the hero still) and
`planetary.json`, the receipt (tooth counts, module, ratio, gap, measured
clearances, motion verdicts and, with `--study`, the sun's stresses).

## Where every size comes from

The only inputs are `--diameter` (the ring's outside, 70 mm by default) and
the print process (`--process fdm --material pla`, or `petg`). The rest
derives:

- **The gap**: `print_allowance` kind `gap` for that process and material
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
- **Herringbone**: each half is a Loft through eight sections of the same
  outline, each turned along a 30° helix (`phase_deg` grows with height in
  proportion to 1 / pitch radius, signed so every section is the same rolled
  pose), and the halves are unioned. At four sections a half, the skin
  strayed far enough from the helix between sections that a sun and planet
  clearing 0.28 mm at every section touched between them.
- **Phasing** (`phases()`): the sun's tooth 0 points along +X. Each planet
  takes the half pitch less the sun's offset at its angle, and the ring
  cutter takes the planet's tooth at the ring. The script asserts all three
  planets ask the ring for the same phase.

## What is checked, every run

- `check_geometry` on the print part is `verified`.
- **Every meshing pair's exact B-rep distance is at least the gap a side
  less 0.01 mm.** The lofts come out about 5 µm short: 0.095 mm (sun) and
  0.097 mm (ring) against PLA's measured 0.1.
- **The STL has five shells**: two bodies that touch would fuse into one.
- **The assembly turns.** The `sun`, `planet` and `ring` parts are placed on
  a kinematic carrier (a part of frames only), with the sun driving the
  carrier and each planet through `gear` relations at signed ratios
  (carrier `Zs/(Zs+Zr)`, planet `-(Zs/Zp) Zr/(Zs+Zr)`). Two motion studies
  check for clashes: one mesh cycle (every pair through every phase of its
  mesh) in 24 samples, and one whole orbit of the planets in 240. Both must
  be clean. Mesh clash mode is fine-grained enough: put one planet half a
  tooth out, or only 0.4 mm along its pitch circle, and the cycle study
  fails.

`--study` adds a static study of the sun, the weakest gear. The knob's flats
are held. Each planet's mesh carries a third of the tangential force from a
firm 60 N pinch on the knob, applied to that tooth's driving flank in both
halves of the V. Material is PLA (Prusament's datasheet), at 0.4 module
elements: 111k tets.

The solve goes to the GPU (docs/simulation/README.md#the-solver): about
30 s, or two minutes on the CPU's iterative solver. At 70 mm and 0.39 N m on
the knob, the factor of safety against yield is about 13. The peak (3.6 MPa)
is in the sharp inside corner where the hex knob meets the gear's face, a
re-entrant corner whose stress grows as the mesh is refined (2.7 MPa at 0.6
module). It is not at a tooth root: a hand cannot strip these teeth.

The generator test `generator_planetary`
(generators/README.md#the-big-generators-as-tests) runs the default build
without the still.

## Printing it on a P1S

1. **The gap is the printer's.** PLA's 0.1 mm a side was measured on a P1S
   in K5 PLA: a set modelled at the community's 0.25 printed loose, and
   `print_coupon.py --gap-only --gap-center 0.1 --step 0.025` showed 0.075
   freeing only under force and 0.1 sliding. On another printer or
   filament, print `print_coupon.py --gap-only` first: the tightest disc
   that slides out by hand is its gap. If it differs, correct
   `src/core/PrintAllowance.cpp` from it (citing the print) and regenerate.
   Slice the coupon with the same preset as the gears, and keep the STL
   chords fine (both generators export at a tenth of the gap or less): at
   0.1 mm chords the facets took 0.03 mm a side off every gap, and the
   slicer's gap closing shut the narrowest.
2. Slice `planetary.stl` flat on the bed as it comes, **no supports**, 0.2 mm
   layers.
   - **No brim**, or an outer brim only. A brim between the gears welds them.
   - The first layer is where a print-in-place part fuses, so keep elephant
     foot compensation on.
3. Off the bed, hold the ring and turn the knob firmly once to break any
   first-layer whiskers. Then it should spin.
