# A load-tested PLA wall hook

Loböle's static FEM has been checked against closed-form beams and
cylinders, never against a part that broke. This dogfood makes one to
break: a printed wall hook whose predicted failure load, per print
orientation, the user then measures with a luggage scale.

## Why a hook, not a shelf bracket

A hook is one cantilever with one load point, and a luggage scale's hook
sits in its saddle: the load is where the study puts it, and its peak is
what a peak-hold scale reads. A shelf bracket shares its load with a shelf
and the wall, and fails at a screw as often as in the plastic, so the test
would measure the wall. The hook's support is made the strong end on
purpose (below), so it breaks at the arm's root, where the prediction is.

## The part

A J profile in the XZ plane (the wall is x = 0, z up), extruded across its
width along Y:

- **back plate** against the wall, a countersunk wood-screw hole above the
  arm and one below;
- **arm** straight out from the plate, a fillet at its root top and bottom;
- **lip** rising at 45° from the arm's end, a fillet in the saddle where
  the scale's hook sits.

Inputs: `--load-kg` (the rated load, 5), `--reach` (the saddle's clear
depth off the wall, what hangs needs it), `--screw` (the screw's diameter)
and `--profile` (the print profile, as in planetary). Everything else
derives:

- width: twice the screw head (a 90° countersunk head is two diameters);
- arm thickness from the beam formula, so the WEAK orientation (below)
  fails at `--design-factor` (2) times the rated load:
  `t = sqrt(6 F L / (w σ_interlayer))`;
- root fillets half the arm's thickness, so the root's stress is the
  fillet's and does not grow with the mesh;
- plate thickness 1.25 times the arm's and the screws two head diameters
  from the arm, so the plate is not the weak link;
- the clearance holes from `print_allowance` kind `hole`.

The lip's 45° is what prints the upright orientation without support.

## Two orientations, one solve

- **flat**: the profile on the bed, the width up. Layers lie in the plane
  of the J, so every bending stress runs along them; the in-layer tensile
  strength governs.
- **upright**: the back plate on the bed, the arm standing up. The layers
  stack along the arm, so the root's bending stress pulls them apart; the
  interlayer strength governs.

The generator exports both STLs, each laid on the bed.

The stress tensor does not depend on how the part was printed, only which
allowable applies to which component does. So one solve answers both, if
the results keep the tensor. Today they do not: the solver averages the
full tensor to the nodes and keeps only von Mises (`FemSolver.cpp`), the
material has one strength (`yieldMPa`), and nothing knows a layer.
**That gap is fixed in this round**, because without it the upright
prediction is a number the app cannot produce:

1. `FemResults` keeps the nodal stress tensor (six components, float like
   every other field), saved with the result (format bump, golden, version
   history row).
2. `MaterialProperties` gains `interlayerMPa`: Prusament PLA's datasheet
   (the source the PLA row already cites) gives an interlayer adhesion of
   17 ± 3 MPa against a horizontal yield of 51; PETG's row gets its own.
   0 = unknown, like the other fields.
3. `study_results` adds the largest tensile principal stress
   (`max_principal_mpa`, value, at, body). PLA breaks brittle, in tension,
   so the largest tensile principal stress is the criterion; von Mises is
   symmetric in tension and compression and stays as the yield check.
4. `study_results` and `probe_study` take an optional `layer_normal`
   [x,y,z] (the build direction in the part's frame) and report the
   largest TENSILE stress normal to the layers, n·σ·n, its place, and a
   factor of safety against `interlayerMPa` (`interlayer_factor_of_safety`).
   A parameter rather than a stored study field: one solve then answers
   every orientation. Refused on an assembly study (whose part is laid
   which way?) and on a material with no interlayer strength.
   `probe_study` also reports the face's largest tensile principal stress.

The study stays isotropic in stiffness: PLA's modulus across layers is
within 10-20 % of along them (Bambu PLA Basic's datasheet: 2.16 against
2.68 GPa), and the hook's stresses are statically determined by the moment,
so stiffness anisotropy moves them little. Failure is where FDM is
anisotropic, and the allowables carry that.

## What "predicted failure load" means

The study runs at the rated load. Linear static scales, so a stress σ per
newton gives a failure load allowable / σ. Per orientation:

- flat: the root fillet's largest tensile principal stress against the
  in-layer strength (the material's 48 MPa);
- upright: the root's tensile stress normal to the layers against 17 MPa,
  and the in-layer one as well; the lower load governs.

Both are read on the root fillet faces with `probe_study` (the global peak
is reported too, with where it is). The card also gives an upper bracket:
an ISO 178 bending bar of the same PLA breaks at a linear-elastic stress of
83 MPa, not 51 (the same datasheet), because the outer fibres yield before
the bar breaks. A hook breaking in bending may follow the bar rather than
the tensile number; where the measurement lands between them is the
finding.

## Mesh convergence

Two solves, the second with the root fillets' local refinement and the
global mesh size both halved. The card prints both and their difference;
the run fails if the root stress moves more than 10 %. Small part, CPU
direct solve: seconds each.

## Output and the test card

`scratch/hook/`: `hook_flat.stl`, `hook_upright.stl` (each laid on the bed),
`hook.dsgn`, `hook.json` (the receipt) and `hook_card.md`: per orientation
the predicted failure load in kg (and the bending-bar bracket), where it
breaks, print settings, how to load it, and a table to fill in.

## Files

- `src/core/fem/FemSolver.{h,cpp}`, `FemStudy.cpp`: keep, round and gather
  the tensor; a face's tensile-stress probe.
- `src/core/io/SerializeStudies.cpp`, `SerializationInternal.h`: save it;
  `kVersion` bump; `tests/golden/v<N>-...`; `docs/versioning/format.md`.
- `src/cad/CADMaterials.{h,cpp}`: `interlayerMPa`.
- `src/remote/RemoteSimulation.cpp`: the new result fields, `layer_normal`,
  describe rows.
- `tests/test_fem.cpp`: a cantilever's principal and layer-normal stresses
  against the beam formula, a compressive side reading none, and the
  refusals; serialization round trip.
- `generators/hook/{build.py,README.md}`, `generators/README.md` index,
  `tests/generator_runs.py` and `tests/CMakeLists.txt` (`generator_hook`).
- `docs/simulation/README.md`: principal and layer stresses.

## Verification

- The unit tests above, the headless suite, the full generator run.
- `generator_hook`: build, check_geometry verified, both STLs one
  watertight shell, both solves, convergence within 10 %, the card written.
