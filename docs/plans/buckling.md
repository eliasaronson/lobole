# Buckling studies, solved in-house

## The problem

A part that fails by buckling passes a static study with a comfortable
factor of safety. Slender struts, thin printed walls, ribs, webs and tubes
all fail this way, and the static answer looks finished. Agents designing
printed parts have repeatedly shipped designs that folded for this reason,
with nothing in the study to tell them.

Linear buckling answers it: by what factor can the load grow before the part
loses stability, and in what shape? It is one generalized eigenproblem on
matrices we already have. So it is solved in-house and ships with no outside
dependency. CalculiX, already on main as a second solver for static studies
(docs/simulation/README.md#calculix), becomes buckling's test oracle, and is
also offered as `solver: calculix` for a second answer. Everything past
linear stays CalculiX's, in docs/plans/calculix-bridge.md.

## The shape of the fix

### The analysis

`AnalysisType` gains `Buckling` (src/core/fem/FemStudy.h). A buckling run:

1. **Solves the base state.** This is the study's linear static solve at
   factor 1, on whichever solver a static study would use (direct, PCG or
   GPU). A pin's contact set comes from the `SolveWithPins` iteration and is
   then held bilaterally. Linear theory scales the load without turning it,
   so the set holds at every factor, and the result says so.
2. **Assembles the geometric stiffness K_G from that stress.** For each tet10
   at each of `kTetGauss`'s four points, with stress tensor σ, each pair of
   nodes contributes ∫ (∇N_a · σ · ∇N_b) dV to the 3×3 identity block.
   CalculiX's C3D10 uses the same rule, so the same mesh gives the same
   matrix. A held DOF is removed from K_G exactly as it is from K.
3. **Solves −K_G φ = μ K φ for the largest |μ|**, with λ = 1/μ. Written this
   way, K is the B matrix, symmetric positive definite once supported, and
   K_G may be indefinite. Spectra's `SymGEigsSolver` in Cholesky mode takes
   it as it is, and the factor is the `SimplicialLDLT` of K a modal study
   already makes (FemSolver.cpp). The largest positive μ values give the
   smallest positive factors, which govern. The most negative μ gives the
   factor under the reversed load.

`num_modes` is reused as the number of factors and defaults to 3: a
symmetric section has two equal first modes. `load_cases` is refused on a
buckling study, because a factor scales one load set. Gravity is part of
that load set, and the result says it is scaled with the rest. A study with
no compressive stress anywhere, so that no positive μ exists, answers
`buckling_factor: null` with the reason "nothing in this load buckles". It
does not pick a number.

**Memory.** The direct factor is the limit, as for a modal study. The
estimate a modal study uses refuses a run that will not fit, and names both
numbers.

### What the result says

Each body gets:
- `buckling_factors`: every factor found, signed, ascending by magnitude.
- `buckling_factor`: the smallest positive factor.
- mode shapes, stored as modal ones are (`ModeShape` gains `factor` beside
  `frequencyHz`), shown with the deform slider and the mode animation.

**The governing number.** Elastic buckling alone over-promises in three
ways, and each correction is derived from the part, never a constant:
- **Imperfection sensitivity.** A curved wall buckles at a fraction of its
  linear load. The knockdown comes from NASA SP-8007,
  γ = 1 − 0.901(1 − e^(−φ)) with φ = √(R/t)/16. R is the radius of the
  cylindrical, conical or spherical B-Rep face that carries the first mode's
  peak motion, and t is the wall thickness there, measured through the solid
  along the inward normal. On a flat face or a member, γ = 1. Plates and
  struts are not imperfection-sensitive the way shells are, and the
  interaction below covers what imperfection does to a stocky strut. The
  result names the class and γ.
- **Yield.** `yield_factor` = yield ÷ peak von Mises of the base state. For a
  printed material, also `interlayer_factor` = `interlayerMPa` ÷ peak
  across-layers stress.
- **Interaction.** A member near the transition between yielding and
  buckling fails below both. The factor that governs is Rankine–Gordon,
  1/λ_gov = 1/(γ·λ_b) + 1/λ_yield, with λ_yield the smaller of the yield
  and interlayer factors. It is reported as `governing_factor`, with the
  three inputs beside it.

**Honesty flags**, the counterpart of `peak_at_support_edge`:
- `mode_at_load_edge`: the first mode's peak motion lies within the distance
  that `peak_at_support_edge` calls "away" (in mm) of a
  support's or load face's edge. The mode is local crippling where the load
  enters, not member buckling. It names the face.
- **Through-thickness resolution.** A wall the first mode bends with one
  element through it, when the check below found one layer off. It is a
  warning on the factor.
- **Base-state convergence.** It follows the static study's `converge` setting.
  Under `auto` it runs when the solve was
  quick, and it is reported the same way.

The receipt's claim has basis `predicted`, with oracle `in_house/buckle`.
Its note says linear buckling is an upper bound that the knockdown and the
interaction correct for, and that snap-through (an arch or a curved wall
loaded across its curve) needs a nonlinear run (calculix-bridge.md,
Stage 4).

### Solver choice

- `auto` and `in_house` solve buckling here.
- `calculix` writes `*BUCKLE` through the existing deck (`fem::WriteDeck`)
  with the same base-state contact set. It reads the factors from `.dat`
  and the modes from `.frd`, and reports the same keys with oracle
  `calculix/buckle`. It is a second answer, as a static study on CalculiX
  is: the receipt basis does not change.

### Requirements, sweeps, GUI

- `study_result` gains the quantity `buckling_factor`, read as the
  governing factor and used with `at_least`. It carries the `not_converged`
  warning a static peak does. `factor_of_safety` on a buckling study
  reads `governing_factor`.
- Sweeps list a buckling requirement as not swept, with the same reason as
  other study requirements.
- StudyPanel:
  - `Buckling` in the type combo;
  - a "Buckling factors" list in place of "Natural frequencies";
  - selecting a factor shows its mode;
  - the governing factor, its class and the flags are shown as text.
- `set_result_display {mode}` selects a buckling mode.

### Format

The study key `analysis: "buckling"` and the result keys are `vNEXT`.
The result keys are `buckling_factors`, `buckling_factor`, `governing_factor`,
`knockdown`, `yield_factor`, `interlayer_factor` and a mode's `factor`.
Plumbing for the bump:
- raise `kVersionPending`;
- goldens `tests/golden/vNEXT-buckling-study.json` and `.expect.json`;
- a row in format.md's pending table.

## Verification

**Against CalculiX on the same mesh** (`[buckling]`, gated on ccx as
`[calculix]` is). Each pair is run with `solver: auto` and
`solver: calculix`, and the first three factors must agree to 1e-4:
- the column;
- a bracket on a pin;
- a torque plus a remote load;
- a bonded two-material assembly.

The same element and rule are used on both sides, so this measures K_G and
the eigensolve, not the physics.

**Closed forms, in-house only, so they run where ccx is absent:**
- Euler, fixed–free, 10×10×200 steel: within 0.5 % of π²EI/4L². A 10×20
  section: the first two factors in the ratio 4.
- A wide cantilever plate: within 2 % of the plate-stiffness value
  Et³/12(1−ν²).
- A thin cylinder in axial compression: λ_b against the classical
  Et/(R√(3(1−ν²))) within 5 %, and the reported knockdown equals
  SP-8007 at that R/t.
- A stocky strut, where yield and buckling are close: `governing_factor`
  is below both and equals Rankine–Gordon.
- Pure tension: `buckling_factor: null` with its reason.

**Through-thickness, measured before anything is built on it.** A 1 mm
plate's first factor is compared at one element through the wall and at two
or more, the latter by a refinement on its faces (refinements go
through Netgen's per-face size, so a fine one holds). If one layer is off by more than 2 %, a buckling study
gets two layers where its first mode bends. That means a refinement on
those faces sized to half the wall, and one re-solve, reported. Otherwise
the warning above is the only change.

**Refusals** (in `refused_calls_smoke` and a C++ test): load cases on
buckling; a memory estimate over the available memory.

**The MCP-only path:** `tests/buckling_smoke.py` builds a column through RPCs
only. It adds a buckling study and a `buckling_factor` requirement, then
reads the factor and `check_requirements`.

**Cost:** a `[slow]` CPU-time check of a buckling run of about 240 k DOF,
measured once and recorded beside the test.

**The dogfood check.** The leaned-on crank studies in
`generators/woodcutter/woodcutter_fem.py` gain a buckling study of the crank
and the handle pin. They report whether either is buckling-limited at the
design lean.

**Docs:**
- docs/simulation/README.md gains a "Buckling" section: the method, the
  governing factor and its three corrections, the flags, and the CalculiX
  cross-check. Its "Not done" list drops buckling.
- calculix-bridge.md loses its buckling stage. Its shipping stage moves
  behind the nonlinear stage, since nothing a user needs requires a bundled
  ccx until then.
- The describe rows for `add_study`, `set_study`, `study_results`,
  `set_result_display` and `check_requirements`.

## Files

- `src/core/fem/FemSolver.{h,cpp}`: K_G assembly, the buckling eigensolve,
  `ModeShape::factor`, buckling result fields.
- `src/core/fem/FemStudy.{h,cpp}`: the type, its refusals, the solve
  branch, the corrections and flags.
- `src/core/fem/Calculix.{h,cpp}`: `*BUCKLE`, reading the factors and modes.
- `src/core/io/SerializeStudies.cpp`, `SerializationInternal.h`,
  format.md, the goldens.
- `src/remote/RemoteSimulation.cpp`; `src/core/Requirements.cpp`,
  `src/remote/RemoteRequirements.cpp`.
- `src/ui/StudyPanel.cpp`.
- `tests/test_fem_buckling.cpp`, `tests/test_fem_calculix.cpp`,
  `tests/buckling_smoke.py`.
- `generators/woodcutter/woodcutter_fem.py`.
- `rpc.py` and `rpc-surface.json`, regenerated.
