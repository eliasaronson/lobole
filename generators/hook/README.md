# A load-tested PLA wall hook

A printed wall hook sized for a load, with a predicted breaking load for
each way it can be printed, made to be broken on a luggage scale. It is the
first check of Loböle's static FEM against a part that physically failed:
until now the solver had been checked against closed-form beams and
cylinders only.

    tools/headless-preview.sh --start <port>
    python3 generators/hook/build.py --port <port> --fresh --profile "K5 PLA on P1S"
    python3 generators/hook/build.py --port <port> --load-kg 8 --reach 40 --screw 5

Output goes to `scratch/hook/` (or `--out`): `hook_flat.stl` and
`hook_upright.stl`, each lying on the bed as it prints; `hook.dsgn` (the
two print parts and the installed `hook` with its study); `hook.json`, the
receipt; and `hook_card.md`, the test card: per orientation the predicted
breaking load in kg and where it breaks, how to print and load it, and a
table to fill in.

## Why a hook

A hook is one cantilever with one load point, and a luggage scale's hook
sits in its saddle: the load is where the study puts it, and the peak is
what a peak-hold scale reads. A shelf bracket shares its load with a shelf
and the wall, and fails at a screw as often as in the plastic, so its test
would measure the wall. The hook's support is made the strong end (below),
so it breaks at the arm's root, where the prediction is.

## The part and where its sizes come from

A J, in the hook's own axes x out of the wall, z up and the width along y:
a back plate with a countersunk wood-screw hole above the arm and one below,
the arm straight out with a fillet at its root top and bottom, and a lip
rising at 45° with a fillet in the saddle. It is one sketch of the outline
(lines and tangent arcs) extruded across the width, and one countersink
Hole feature.

The inputs are `--load-kg` (the rated load, 5), `--reach` (the saddle's
clear depth off the plate, 30 mm: what hangs needs it), `--screw` (the
screw's diameter, 4 mm), `--design-factor` (2) and the print process
(`--profile NAME`, else `--process`/`--material`). The rest derives:

- **Width**: twice the screw head, and a 90° countersunk wood screw's head
  is two diameters.
- **Arm thickness**: from the beam formula, so the WEAK orientation fails
  at the design factor times the rated load: `t = sqrt(6 F L / (w σ))`
  with σ the material's interlayer strength (describe's `materials`), to
  the next 0.1 mm. At 5 kg, 8.1 mm.
- **Fillets**: the root's half the arm's thickness, so the root stress is
  the fillet's and does not grow with the mesh; the saddle's the same; the
  outer corners a quarter.
- **Plate**: 1.25 times the arm (twice as strong in bending) and at least
  the countersink plus two nozzle widths; the screws a head's diameter
  clear of the root fillets and the plate a head's diameter past them.
- **Holes**: the shank plus 0.5 mm and the print allowance table's `hole`
  row; the countersink the head plus the same.
- **Lip**: 45° from the arm, so the upright print overhangs by 45° and
  needs no support; it rises half the reach above the arm.

## Two orientations, one solve

- **flat**: the J on the bed, its width up. The layers lie in the plane of
  the J, so every bending stress runs along them, and the in-layer tensile
  strength governs.
- **upright**: the back plate on the bed, the arm standing up. The layers
  stack along the arm, so the root's bending stress pulls them apart, and
  the interlayer strength governs.

The study is of the installed hook at the rated load: the back face held
(screwed tight to a stiff wall) and the load straight down on the saddle's
fillet. The stress does not depend on how the hook was printed; which
allowable applies to which component does. So one solve answers both: the
generator reads it with `layer_normal` y for the flat print and x for the
upright one (docs/simulation/README.md#printed-layers).

## What "breaks at" means

Linear static scales with the load, so a stress σ at the rated load breaks
the hook at `allowable / σ` times it. Per orientation, the lower of:

- **in-layer tension**: the largest tensile principal stress on the root
  fillets (`probe_study`'s `principal_max_mpa`) against PLA's yield (48 MPa,
  the app's `pla` row: Prusament's 51 ± 3 printed flat). Tensile, because
  PLA breaks brittle, in tension; von Mises would read the compressed root
  as hard as the stretched one.
- **layers pulling apart**: the largest tension across the layers on the
  root fillets (`across_layers_max_mpa`) against the interlayer strength
  (17 MPa, Prusament's interlayer adhesion, 17 ± 3).

At 5 kg on the defaults: **flat 23.5 kg**, in-layer tension; **upright
8.6 kg**, layers pulling apart; both at the root's top fillet. The study
puts 10.2 MPa there at 5 kg, 1.41 times the beam formula's 7.3 at the
fillet's end, a filleted shoulder's concentration; the run fails outside
1-2. The card gives an upper bracket for the flat print: a printed ISO 178
bending bar of the same PLA breaks at a linear-elastic stress of 83 MPa, not
51, because its outer fibres yield before it breaks, and a hook breaking in
bending may follow the bar (38 kg). Where the measurement lands between them
is the finding. There is no bending-bar number across the layers.

The prediction is read on the root fillets because that is where the hook
breaks; if the part's worst tension of either kind is more than 5 % above
the root's anywhere else, the run and the card say where.

**Mesh convergence**: two solves, a third of the arm's thickness with the
root fillets at a quarter of their radius, then both halved (16 k and 49 k
nodes). The prediction moved 2.0 %; the run fails above 10 %. A few
seconds each on the GPU, about 13 s for the whole run headless on the CPU.

## What is checked, every run

- `check_geometry` on each print part is `verified`.
- `check_print` (docs/tolerances/README.md#checking-a-print) on each print
  part: its lowest point is on the bed and it is one watertight piece. Its
  overhang and bridge items go in the receipt's `print`.
- The study finds one back face, two root fillets and one saddle by shape
  and place, or stops.
- The root stress is within 1-2 times the beam formula, and moves under
  10 % between the meshes.

The generator test `generator_hook`
(generators/README.md#the-big-generators-as-tests) runs the whole build.

## Printing and testing it

Slice each STL as it lies, **100 % infill** (the study is of a solid
hook; sparse infill is a different section), 0.2 mm layers, no supports.
Screw it through both holes to a stiff board with countersunk wood screws,
snug, and clamp the board upright. Hang a peak-hold luggage scale's hook in
the saddle and pull straight down, slowly, a few seconds to failure (or
hang a bucket and add water). Record the peak and where it broke in the
card's table, two of each orientation.

## Not modelled

- The screws and the wall: the back face is held rigidly. A real plate
  pivots on its lower edge while the top screw pulls; the root's stress is
  set by the moment and moves little, but a hook that breaks at a screw
  hole says the support was the weak link.
- Plasticity and the layer lines' notches: the study is linear and the
  part smooth. Both are why the bending-bar bracket exists.
- TPU: the material library has no TPU row (and no interlayer strength
  for it), so `--material tpu` is refused, naming the materials that have
  one (pla, petg).
