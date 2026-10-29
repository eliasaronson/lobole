# Thermal studies

Stage 7 of [the CalculiX bridge](calculix-bridge.md): steady heat transfer,
then thermal stress.

## The problem

Nothing in the app knows a temperature. A part beside a motor, a hot end, a
resistor, a lamp or a sunlit window is designed blind:

- **How hot it gets.** No study takes a heat input, a held temperature or a
  surface cooled by air, and no result is a temperature.
- **What heat does to it.** A part clamped while it warms, two materials
  bonded and heated, a shaft in a warm housing: thermal expansion is a load
  the static study cannot apply.
- **Where a printed part stops being a part.** PLA softens near its glass
  transition, in the mid-50s °C. For a printed part near heat that is the
  failure that comes first, long before a stress, and no material row
  carries it.

## The shape of the fix

Three pieces: a **thermal** analysis type (steady conduction), a **thermal
load** on a static study (expansion), and the material data they read.
Thermal buckling comes last, as its own step (below). Transient heat,
radiation, heat across a contact, temperature-dependent properties and a
heated modal study are out of scope (listed under "Not done" when it
lands).

### Steady conduction is solved in-house; CalculiX is its oracle

Steady conduction is linear: `K_T T = Q`, the scalar Laplace problem on the
tet10 mesh the static study already makes. It is solved in-house, with
`solver: calculix` (`*HEAT TRANSFER, STEADY STATE`) as its cross-check and a
second answer on request, the way buckling is.

- **The oracle needs the assembly anyway.** The bridge's rule is that a ccx
  answer is read back and held to OUR matrix (`‖K_T T − Q‖` with the
  print-precision floor, and the energy balance `T·(K_T T − Q)`), and that
  everything after the field (fluxes, heat through each face, the balance,
  the peak) is computed by the code an in-house answer goes through. A
  ccx-only thermal study would still assemble `K_T` and `Q`, recover the
  flux and integrate the flows. The solve is the only part left, and the
  CPU solvers `SolveCases` uses (`SimplicialLDLT` below
  `kIterativeAboveDofs`, the incomplete-Cholesky PCG above it) take a scalar
  `K` as they are. In-house is a few hundred lines; CalculiX-only saves
  only the solve.
- **An oracle is worth most when it is independent.** Same element, same
  Gauss rule, two programs: the agreement measures the assembly, the film
  and the loads, as buckling's seven-digit agreement does. With ccx as the
  only solver there is nothing to agree with but closed forms.
- **It runs where ccx does not.** The `.deb`, the AUR package and Nix leave
  ccx to the distribution; the closed-form tests run on hand-made meshes on
  every CI image. Windows has no Netgen today, so no FEM at all, and gains
  nothing either way now; the day Netgen builds there, thermal works
  without `ccx.exe`.
- **It is cheaper.** ccx solves a heat step as Newton iterations: two
  SPOOLES factorisations of a linear problem, on one core. Measured below:
  6.7 s for 44.5 k nodes, 185 s and 2.29 GB for 338 k.

Thermal stress is the same argument: a thermal strain is a load vector and a
term in the stress recovery, linear, in-house; on CalculiX it is
`*EXPANSION` and `*TEMPERATURE`, so every CalculiX path (a linear static
cross-check, large deformation, plasticity, contact) carries it.

### The thermal study

`type: thermal` (`fem::AnalysisType::Thermal`). It meshes every body that
carries a thermal load, as a static study meshes every loaded body, and
takes the study's `mesh_size`, `refinements` and `converge`. Four load
types (`fem::LoadType`), each refused on any other analysis, and every
structural load refused on a thermal study, naming the types that belong:

| type | on | keys | in the solve |
|---|---|---|---|
| `temperature` | a face | `temperature_c` | held: the face's nodes eliminated, as a fixed support's DOF are |
| `heat_flux` | a face | `power_w` (the total, spread uniformly, as a force is) or `flux_w_m2`, exactly one | `∫ q N dA`, into the part when positive |
| `convection` | a face | `film_w_m2k`, `ambient_c` | `∫ h N Nᵀ dA` into `K_T`, `∫ h T∞ N dA` into `Q` |
| `heat_generation` | a body (`body`, its name; `occurrence` on an assembly) | `power_w` (the body's total) | `∫ (P/V) N dV` |

All four take `region` (a chip's footprint on a board is part of a face).
Internally the solver stays mm–N–s: power in mW, so a conductivity in
W/(m·K) is the same number in mW/(mm·K), a flux or film coefficient per m²
is ×1e-3 per mm², and watts are ×1e3. The conversion is at the RPC and the
file reader, once.

**Refused**, by `add_load`, `set_study`, the panel, the file reader and the
run (`fem::ThermalRefusal`), each naming what is accepted: an unknown type
or key; a temperature below −273.15 °C; a film coefficient that is not
positive; both or neither of `power_w` and `flux_w_m2`; a convection
without `film_w_m2k` or `ambient_c`; load cases, gravity, `nonlinear`,
`shape_from`; on an assembly, a `contact` participant (no conductance
across a contact) and a `rigid` one (it is not a heat sink unless a load
says so). At the run, before meshing: a body whose heat has nowhere to go
(no `temperature` and no `convection`, which leaves `K_T` singular), and a
body whose material has no conductivity, naming the materials that have one.

**No literal defaults.** A held temperature, a film coefficient and an
ambient are not derivable from the part, so none has a default: `add_load`
refuses without them, and the panel's tools start empty and refuse
visibly until a value is typed (the `defaults/*` suite's rule). The
refusal for a missing film coefficient quotes the ranges Incropera's
Table 1.1 gives (free convection in gases 2–25 W/(m²·K), forced 25–250,
liquids higher), so the caller can choose, not so the app can.

### The thermal load on a static study

`set_study {thermal_load: {from_study: <thermal study id>, stress_free_c?}
| {uniform_c, stress_free_c} | null}` (`fem::ThermalLoad`). The thermal
strain is `α (T − T₀) [1, 1, 1, 0, 0, 0]`, `T₀` the stress-free
temperature; its load vector is `∫ Bᵀ D ε_th dV` at the four Gauss points,
with `T` interpolated by the tet10 shape functions (ccx does the same:
below), and the stress is `D (B u − ε_th)`.

- **`from_study` re-solves the heat on the static study's own mesh**, from
  the thermal study's loads, rather than interpolating its stored field
  onto another mesh. The conduction solve costs a fraction of the elastic
  one; the static mesh, its refinements and every step of its converge loop
  each get the temperature that belongs to them; and the input hash takes
  the thermal study's definition, never its result, so there is no digest
  to go stale. On an assembly the thermal study must mesh the same
  occurrences, refused otherwise, naming both sets.
- **`stress_free_c`** defaults, with `from_study`, to the thermal study's
  ambient when every convection in it has the same one: the part was at
  ambient before it was heated. Otherwise it is required and refused
  without, as `uniform_c` always requires it.
- **Every case** of a study with load cases carries it, as gravity does.
  Refused on modal and topology studies (buckling takes it, below), with a snap-through run
  (its seed is an in-house buckling mode), beside a contact participant,
  and on a body whose material has no expansion coefficient.
- **On CalculiX** the temperature comes from ccx's own heat run on the same
  mesh (checked against `K_T` as above), written into the static deck as
  `*EXPANSION, ZERO=<T₀>`, `*INITIAL CONDITIONS, TYPE=TEMPERATURE` at `T₀`,
  and `*TEMPERATURE` per node in each step. A nonlinear step ramps it with
  the load (measured below); a plastic run's unloading step returns it to
  `T₀` (`*TEMPERATURE, OP=NEW`), so the residual deformation is what heating
  and cooling left. The linear answer's residual check is unchanged: `f`
  is ours, thermal part included.
- **The nonlinear equilibrium check** divides the imbalance by the load
  carried, which a thermal load alone makes zero (it is self-equilibrated).
  Its scale becomes the larger of the load carried and the sum of the
  supports' reaction magnitudes.

### Material data

`CADMaterials::MaterialProperties` gains `conductivityWmK`,
`expansionPerK` and `serviceMaxC` with `serviceBasis` (what the number is,
e.g. "heat deflection temperature, 0.45 MPa, ISO 75-2"); `fem::Material`
carries the first two. 0 means unknown, as for every property. A value
enters the library only with its source cited beside it, as hardening
curves did; a thermal study or thermal load on a material without one is
refused by name (`fem::ThermalMaterialRefusal`), listing the materials
that have it. Specific heat is not added: it is transient's, which is out
of scope.

| material | conductivity W/(m·K) | expansion 1/K | source |
|---|---|---|---|
| `steel` | 53.3 at 20 °C | 12e-6 | EN 1993-1-2 §3.4.1.3 (λ = 54 − 3.33e-2 θ); EN 1993-1-1 §3.2.6 |
| `stainless_steel` (1.4301) | 15 | 16e-6 | EN 10088-1 (physical properties annex); EN 1993-1-4 |
| `aluminum_6061_t6`, `aluminum_6082_t6` | 191.4 at 20 °C | 23e-6 | EN 1999-1-2 §3.3.1.3 (6xxx: λ = 0.07 θ + 190); EN 1999-1-1 §3.2.5 |
| `pla`, `petg` | the resin maker's datasheet, named as bulk | the same | the resin datasheets (NatureWorks Ingeo for PLA; the PETG resin maker's), transcribed when it lands |

**Generic `aluminum` leaves the library.** Its conductivity runs from 120
to 237 W/(m·K) across alloys, so no one value is right, and the graded
rows say which alloy a part is. A document naming it is refused at load,
naming the graded rows (no migration: there are no users yet); every
generator, fixture and test that used it moves to `aluminum_6061_t6` or
`aluminum_6082_t6`.

**Printed plastics are bulk values, and the result says so.** A printed
part conducts less than its resin (air between roads and layers), so a
study on PLA or PETG carries a warning that its hot spots run hotter than
predicted and its gradients steeper. No measured printed-specimen value
is substituted for a cited one.

Each value is transcribed from the cited clause when it lands; the numbers
above are what those clauses are expected to give. Copper, brass, bronze,
titanium, the plastics and the woods get none until one is cited (wood
conducts differently along and across the grain, which an isotropic study
cannot hold).

**Service temperature.** `serviceMaxC` is the temperature a material stops
holding its room-temperature stiffness and strength, which for an amorphous
plastic is where it softens. PLA and PETG take their datasheet's heat
deflection temperature at 0.45 MPa (ISO 75-2 method B): Prusament's
datasheets, the same documents their interlayer strength cites (the mid-50s
°C for PLA, the high 60s for PETG, read from the datasheet version cited).
It belongs here because the thermal study is what knows the temperature.
Metals get none: their reduction factors start well above where the
printed parts fail (EN 1993-1-2's yield factor is 1.0 to 400 °C), and a
number without a source is not added.

### Thermal buckling

The last step, landed only if it stays small: the in-house buckling study
gains `thermal_load` as its base state's load (the base state is a static
solve, which already takes it), so a clamped part's critical temperature
rise is its buckling factor times the applied rise. Verified against the
clamped bar's closed form (ΔT_cr = π²I/(αAL_e²)) and against ccx
`*BUCKLE` with the temperatures in the deck. If it grows past the base
state's load and those two tests, it is cut and listed as the next step.

The input hash takes a body's conductivity, expansion and service limit
only when the study is thermal or carries a thermal load, so every other
study keeps its hash.

### Results

Per thermal body (`StudyBodyResult::Thermal`, saved with the result):

- **The field.** `FemResults::temperature` (°C) and `heatFlux`
  (`−k ∇T`, per node, averaged by element volume as the stress is), kept on
  the boundary surface as float by `KeepWhatIsRead` like a static field.
- **Heat through each load** (`flows`, one row per thermal load, W, signed
  out of the part): a held face's from `K_T T − Q` at its nodes, each node
  counted once (the static reaction rule); a convection face's
  `∫ h (T − T∞) dA`; a flux face and a source their applied power, exactly.
  ccx's own `RFL` totals are not used: a set's total includes the set's own
  nodal heat load (measured below), so it is not the heat through the face.
- **The balance** {in_w, out_w, imbalance}: in-house held to 1e-6 of the
  throughput; on CalculiX within the floor the read-back precision puts
  under it, as the static residual is. Past it the answer is refused.
- **Peaks.** `max_temperature_c` and `min_temperature_c` {value, at,
  body}, `max_heat_flux_w_m2`; per body against its material,
  `temperature_margin_k` = `serviceMaxC` − the body's peak (absent where the
  material has no limit), with its basis.
- **A settled peak.** The converge loop
  ([a peak that has settled](../simulation/README.md#a-peak-that-has-settled))
  runs on the peak temperature RISE, the peak over the lowest held or
  ambient temperature: a 2 % change in °C is an offset's artefact. The face
  refined is the peak node's, as for stress (an interior peak, a heat
  source's, takes its nearest boundary face). Heat flux peaks at a
  re-entrant edge, which never settles, so it is reported and not chased.

A static study with a thermal load reports `temperature` {min_c, max_c,
stress_free_c, from_study?} and warns, per body, when its peak is above its
material's `serviceMaxC`: the stiffness and strength it was solved with are
room temperature's and do not hold there.

**Requirements.** `study_result` gains `max_temperature` (°C, `at_most`)
and `temperature_margin` (K, `at_least`), read from a thermal study and
refused on any other, the margin refused where no body has a service limit.
Both carry `not_converged` the way a stress peak does. A printed part beside
a heat source is then one thermal study and `temperature_margin at_least
10`, its receipt naming the datasheet. The receipt claims "peak temperature
below the material's service temperature", oracle `in_house/heat` or
`calculix/heat`, basis predicted.

**Display.** `set_result_display` and the panel gain `temperature` and
`heat_flux` (`AppState` `Field::Temperature`, `HeatFlux`): the scalar the
`FieldRenderer` already takes, no deformation, the legend in °C or W/m².
No renderer code changes.

### GUI

The Simulation window: **Thermal** in the analysis combo; on a thermal
study, **Temperature**, **Heat flux**, **Convection** and **Heat source**
(click a face of the body) tools with their fields, empty until typed; the
results list the peak, the margin, each load's heat and the balance. On a
static study, **Thermal load**: none, uniform, or a thermal study, with the
stress-free temperature seeded where it is derived.

## Measured

ccx 2.23 (the nixpkgs build) on hand-written decks: a 100 × 10 × 10 mm steel
bar, k = 50 W/(m·K), a structured tet10 mesh at 5 mm (1 025 nodes), the
same element and node order the bridge writes.

| case | ccx | closed form |
|---|---|---|
| held 100 / 20 °C at the ends | heat through each end 4 000.000 mW; flux 40.000000 at every point | k A ΔT / L = 4 000 |
| internal generation 0.01 mW/mm³, ends at 20 °C | middle 20.25000 | T₀ + q L² / 8k = 20.25 |
| 0.5 mW/mm² into one end, the other at 20 °C | 21.00000 at the loaded end | T₀ + q L / k = 21 |
| a fin: base at 100 °C, every other face h = 25 W/(m²·K) to 20 °C | 65.90889 at mid-length, 55.65738 at the tip | 1-D fin with a convecting tip: 65.855 (+0.08 %), 55.609 (+0.09 %); Bi = 0.0025 |
| clamped along its axis (symmetry planes across), heated 100 K | σxx −240.0 at every point, σyy 3e-12 | −E α ΔT = −240 MPa |

- **Linear and quadratic temperatures are exact** on tet10: the slab, the
  flux and the generation cases come back to the digits printed, which is
  what the in-house tests can demand.
- **ccx's `RFL` totals include the set's own nodal load.** The generation
  case's ends total 49.167 mW each, not 50: the nodes on each end carry
  their share of the body's generation. The fin's base reads 4 997 mW
  against 1-D theory's 5 066 for the same reason. Hence flows computed
  in-house.
- **Temperature at the Gauss points is the tet10 interpolation.** A single
  element held at every node, one midside node at 100 °C: ccx's stresses at
  the four points are −64.72136, −64.72136, −15.27864, −15.27864, exactly
  `−E/(1−2ν) α · 100 N₅(ξ)`.
- **Sequential equals coupled.** The held 100 / 20 °C bar clamped along its
  axis, as `*COUPLED TEMPERATURE-DISPLACEMENT, STEADY STATE`, as
  `*UNCOUPLED …`, and as a `*STATIC` step whose `*TEMPERATURE` lines are the
  heat run's NT: displacements identical to the last digit, total
  reaction 9 600 N = E α (T̄ − T₀) A. So a static deck with the temperature
  written into it is the whole of steady thermal stress; `*TEMPERATURE,
  FILE=<frd>` reads the same, and is not needed.
- **A nonlinear step ramps the temperature with the load**: at a quarter of
  an NLGEOM step the clamped bar carries 5 999 N of its 23 986.
- **What ccx warns.** A `*HEAT TRANSFER` step without its time line warns
  "no time increment nor step is specified"; the deck writes `1., 1.`.
- **Cost** (a 100 × 20 × 20 bar, held ends): 44 541 nodes, 6.7 s, 187 MB;
  337 881 nodes, 185 s, 2.29 GB, two Newton iterations each, one core.
  `fem::CalculixHeatMemoryEstimate` is calibrated from a hidden
  `[calculix-heat-calibrate]` case as the static estimate was, and a run
  past `MemAvailable` refused the same way.

## Files

- `src/core/fem/FemThermal.{h,cpp}` (new): `K_T`, the film, flux and
  generation terms, the scalar solve on the existing solvers, flux
  recovery, flows and balance, and the thermal load vector for a static
  solve.
- `src/core/fem/FemSolverInternal.h`: a one-DOF-per-node `DofMap`; the
  thermal strain in the tet10 stress recovery.
- `src/core/fem/FemSolver.{h,cpp}`: `Material`'s conductivity and
  expansion, `FemResults::temperature`/`heatFlux`,
  `StudyBodyResult::Thermal`, `StudyRuntime`'s thermal peaks,
  `KeepWhatIsRead`, `SolveCases` taking a nodal temperature field.
- `src/core/fem/FemStudy.{h,cpp}`: `AnalysisType::Thermal`, the four load
  types and their keys on `Load` (`body` for a source), `ThermalLoad` on
  `Study`, `ThermalRefusal`, `ThermalMaterialRefusal`, planning and meshing
  thermal loads, the run's dispatch, the converge loop's temperature rise.
- `src/core/fem/AssemblyStudy.cpp`: thermal on meshed and bonded
  participants, the refusals for the rest.
- `src/core/fem/Calculix.{h,cpp}`: `HeatOnCalculix` (the heat deck:
  `*CONDUCTIVITY` per domain, `*BOUNDARY` 11, our nodal heat as `*CFLUX`,
  `*FILM` on each facet's tet face `F1`–`F4` by the map the follower
  pressure uses, `*NODE PRINT` NT), `ReadDatTemperatures`, the `K_T`
  check; `WriteDeck`'s `*EXPANSION`, initial and step temperatures and the
  unloading step's return; the heat memory estimate; the equilibrium
  check's scale.
- `src/core/fem/FemInputKey.cpp`: the thermal source study and the
  materials' thermal numbers, only where read.
- `src/cad/CADMaterials.{h,cpp}`: the three properties, cited.
- `src/core/io/SerializeStudies.cpp`: the study type, the loads' keys,
  `thermal_load`, the result's arrays and flows.
  `src/core/io/SerializationInternal.h` (`kVersionPending`),
  `docs/versioning/format.md`'s pending row, golden
  `tests/golden/vNEXT-thermal.json` + `.expect.json`.
- `src/remote/RemoteSimulation.cpp`: the `add_study`, `add_load`,
  `set_study`, `study_results`, `set_result_display` and
  `export_study_inp` rows and handlers; describe's `materials`.
- `src/core/Requirements.cpp`, `src/remote/RemoteRequirements.cpp`: the two
  quantities.
- `src/ui/AppState.h`, `src/ui/StudyPanel.cpp`,
  `src/ui/SimulationActions.cpp`: the tools, the results, the fields.
- `tests/rpc_units_gate.py`: the stems `temperature`, `flux`, `power`,
  `film`, `conductivity`, `expansion`, and the suffixes `_c`, `_k`, `_w`,
  `_w_m2`, `_w_m2k`.
- The python client's `rpc.py` and `rpc-surface.json`, regenerated.

## Verification

**Closed forms, in-house** (`tests/test_fem_thermal.cpp`, `[thermal]`,
hand-made meshes where the geometry allows, so they run on every CI image):

- the slab held at two temperatures: T linear and each face's heat
  `k A ΔT / L` to 1e-9; the generation slab's `q L²/8k` and the flux slab's
  `q L / k` to 1e-9; every balance to 1e-9;
- the fin above, against the 1-D fin with a convecting tip: temperatures
  within 0.5 %, the base's heat (computed in-house) within 2 %;
- a pipe on a Netgen mesh, the bore held, the outside convecting: the heat
  per length `2π (Tᵢ − T∞) / (ln(rₒ/rᵢ)/k + 1/(h rₒ))` within 1 %, and a
  solid cylinder with a source, `T(0) − T∞ = q R²/4k + q R/2h`;
- a bar heated uniformly and clamped along its axis: `σ = −E α ΔT`; a cube
  held on every face: `−E α ΔT / (1 − 2ν)`; a free bar under a linear
  temperature across its depth, held statically determinate: no stress,
  and curvature `α ΔT / h`;
- the converge loop settling a source's peak rise on its face; the
  refusals; the result's round trip and the input hash moving on a thermal
  input and nothing else (`tests/test_study_inputs.cpp`).

**CalculiX on the same mesh** (`tests/test_fem_calculix.cpp`, `[thermal]`):
a thermal study solved `auto` and `calculix`, temperatures and flows agreeing
at the precision ccx prints; a static study under a `from_study` thermal
load the same way; a steel bar clamped along its axis, heated past yield
with plasticity and cooled again: plastic strain
`(α ΔT − f_y/E) / (1 + H/E)` and the residual stress it leaves, against
the library's hardening slope; a deck whose conductivity differs by 0.1 %
refused by the `K_T` check.

**Over the RPCs alone** (`tests/thermal_smoke.py`, skipping on a build
without Netgen, its CalculiX half gated on `ping`'s `features.calculix`): a
PLA block with a heat source and a convecting face; the run, the peak, the
margin; a `temperature_margin` requirement unchecked then checked, with its
receipt; a static study loaded `from_study`; the temperature field
displayed; the exported heat deck run through ccx.

**Refusals** in `refused_calls_smoke` (each changing nothing): an unknown
thermal load type and key, a structural load on a thermal study and a
thermal one on a static study, a temperature below absolute zero, both
`power_w` and `flux_w_m2`, a convection without its film coefficient, load
cases on a thermal study, `thermal_load` on a modal or topology study
(and on a buckling study if thermal buckling is cut), a document naming
generic `aluminum`, a missing `stress_free_c` that cannot be derived, a material without
conductivity at the run, a body whose heat has nowhere to go.

**GUI** (`sim` category): each thermal tool refusing visibly until it has
its value and placing once it does; a thermal result's field and legend.

**Cost** (`[slow]`): an in-house thermal study of about 300 k nodes, CPU time
bounded at twice what it measures on an E-core, as buckling's check is.

**Docs.** docs/simulation/README.md gains a Thermal section (the study, the
thermal load, the materials, what is measured) and the RPC rows; "Not done"
loses thermal and lists what this leaves out; docs/requirements/README.md
gains the two quantities. Thermal is the bridge's last stage, so
docs/plans/calculix-bridge.md goes with this plan once what is still true of
it is in the simulation page.
