# CalculiX as a study solver

## The problem

Linear static, modal and topology studies are solved in-house and stay that
way. Everything past linear is not built: buckling, large deformation,
contact that separates or slides, plasticity and thermal
(docs/simulation/README.md#not-done). That section already names an
external solver as the way to add them, rather than growing ours. A part
that fails by buckling (a slender strut, a thin printed wall, a tube frame
member) currently passes a static study with a comfortable factor of safety.
That is the worst kind of wrong answer, because it looks finished.

CalculiX (`ccx`) does all of these on the element we already mesh. Its
C3D10 tetrahedron has the same node order as `FemMesh::tets` (corners 0–3,
then midsides (0,1), (1,2), (2,0), (0,3), (1,3), (2,3)) and the same
4-point Gauss rule as `kTetGauss` (src/core/fem/FemSolverInternal.h). Its
nodal stress order, xx yy zz xy yz zx, is our Voigt order. Its units are
whatever the deck is written in, so our mm–N–MPa–t system passes through
unchanged.

The bridge is a solver behind the existing study API, not a second API.
Studies, loads, supports, materials, the Netgen mesh, the results shape,
requirements, the result display and `run_study`/`study_results` all stay
what they are. A study gains a type (`buckling`, then nonlinear options on
`static`) and a `solver` choice. A study that needs CalculiX on a machine
without it is refused, and the refusal says how to install it. A CalculiX
failure is refused with CalculiX's own error. Nothing falls back to the
in-house solver or to another analysis.

## What was measured

These numbers are the basis for the plan. They were taken against the
existing build and the nixpkgs ccx, with no code changes.

**Availability**

| where | ccx | how |
|---|---|---|
| nix (our nixpkgs) | 2.23, from the binary cache | `calculix-ccx` (the old `calculix` attribute now throws "renamed"). Deps: spooles, arpack, blas, lapack, libgfortran. 5.6 MB binary, 240 MB closure |
| Ubuntu 24.04 noble | 2.21-1 | `apt install calculix-ccx` (universe) |
| Ubuntu 26.04 resolute | 2.21-1build1 | same |
| Windows | 2.23 | dhondt.de `calculix_2.23_4win.zip`. `ccx_static.exe` (42 MB) bundles SPOOLES and PaStiX statically. The `ccx_dynamic*` variants need MKL's `mkl_rt.2.dll`, which is not shipped. Not cross-buildable from nixpkgs: `pkgsCross.mingwW64.calculix-ccx` is refused as an unsupported system. Running it under Wine was not done: the agent's sandbox denied running the downloaded binary |
| AppImage | none | no host tool is bundled today (`proc::FindOnPath` + `PipedChild` look up ffmpeg on PATH) |
| Flatpak | none | the sandbox hides a host install, so ccx must be built as a module |

**Licence**
- ccx is GPL-2.0-or-later (nixpkgs meta, and ccx's start-up banner).
- SPOOLES is public domain, ARPACK is BSD-3, and libgfortran is GPL-3 with the runtime exception.
- Loböle runs ccx as a separate process that talks to it only through files (an `.inp` in, `.frd`/`.dat` out). That is aggregation, not a combined work, so it has no effect on Loböle's AGPL-3.0-or-later licence.
- Shipping the binary in an AppImage or installer is allowed if:
  - `LICENSES/GPL-2.0.txt` ships with it (it already does, for the fastener tables);
  - we offer the exact corresponding source ourselves. GPL-2 §3(a)/(b) puts that on whoever distributes; a link to upstream counts only for non-commercial redistribution under §3(c). In practice, the ccx source tarball becomes a release asset next to ours.

**Capability** (ccx 2.23 manual)
- `*BUCKLE`: buckling factors are written to `.dat` and mode shapes to `.frd`; `PERTURBATION` adds a preload.
- `*STEP, NLGEOM`: large deformation.
- `*CONTACT PAIR` with `*SURFACE INTERACTION`: penalty or mortar, face-to-face or node-to-face, with friction.
- `*PLASTIC`: isotropic or kinematic hardening.
- Tension-only and compression-only materials.
- `*HEAT TRANSFER`, `*COUPLED TEMPERATURE-DISPLACEMENT`.
- `*EQUATION` and `*TRANSFORM`: arbitrary-direction supports.
- `*COUPLING` + `*DISTRIBUTING`: a remote load as RBE3.
- `*TIE`.

**Accuracy and cost**
- Test piece: a 10×10×200 mm steel column (E 200 GPa, ν 0.3) clamped at the base.
- Meshes: ours is Netgen inside the app; ccx's is gmsh from the exported STEP. They are different meshes of similar size, because there is no mesh export yet (Stage 1 adds one).

| case | in-house | ccx 2.23 | closed form |
|---|---|---|---|
| tip deflection, 100 N lateral, 2.5 mm mesh | 1.59761 mm (10 135 nodes) | 1.59788 mm (13 154 nodes) | 1.6000 mm (FL³/3EI) |
| Euler buckling, fixed–free, 2.5 mm mesh | — | 10 286.9 N (+0.06 %) | 10 280.8 N (π²EI/4L²) |
| same, 1.25 mm mesh | — | 10 284.3 N (+0.03 %); mode 3: 91 070 N, 9× theory −1.5 % (shear) | |
| static wall time, 2.5 mm mesh | 5.8 s with mesh | 1.9 s, 150 MB | |
| static, 1.25 mm mesh | 15.2 s with ~4 s mesh (93 k nodes) | 20.7 s, 1.44 GB (79 k nodes, 236 k DOF) | |
| buckling, 1.25 mm mesh, 3 factors | — | 48 s, 1.6–2.2 GB | |

- The nixpkgs build factors on one core ("Using 1 cpu for spooles") whatever `OMP_NUM_THREADS` / `CCX_NPROC_EQUATION_SOLVER` say: SPOOLES is not built multithreaded.
- Memory grows faster than the DOF count, as it does for any direct solver. Roughly 6 KB per DOF for static and 7–10 KB per DOF for buckling at this size.

## The shape of the fix

### The solver, located and versioned

New `src/core/fem/Calculix.{h,cpp}`. `FindCalculix()` looks in this order:
1. the `calculix_path` setting;
2. `LOBOLE_CCX`;
3. a bundled `ccx` beside the executable (`<prefix>/lib/lobole/ccx`, or `ccx.exe` beside `lobole.exe`);
4. `proc::FindOnPath("ccx")`.

It runs `ccx -v` once per path and stamp, parses "This is Version 2.NN", and refuses anything below 2.21, the oldest one we test.

`ping` gains `features.calculix: {path, version}` or `null`, the same way it reports ffmpeg (`RemoteDispatch.cpp`). The smokes gate on that.

The refusal when ccx is missing is one sentence naming every route:
- Ubuntu/Debian: `sudo apt install calculix-ccx`;
- Nix: `calculix-ccx`;
- Windows: the dhondt.de zip, or the bundled copy if Stage 3 ships one;
- or the `calculix_path` setting.

The refusal comes from `run_study`, before any job starts. `add_study` and opening a document never need ccx, so a project with a buckling study opens and shows its saved result on any machine.

### Study type and solver choice

- `AnalysisType` gains `Buckling` (src/core/fem/FemStudy.h).
- `Study` gains `solver`: `auto | in_house | calculix`. It is absent from the file when `auto`.
- What `auto` means:
  - in-house for static, modal and topology;
  - CalculiX for buckling and for any nonlinear option (Stage 4 on).
- What is refused, with the reason named:
  - `in_house` on buckling or nonlinear;
  - `calculix` on modal or topology. Modal could map to `*FREQUENCY` later; until it does, it is refused.
- `calculix` on a linear static is allowed. It is the cross-check (below), and it is how the agreement tests run through the real pipeline.
  - Its result is a second, independent answer, not a stronger one.
  - The receipts keep the basis an in-house run has (`predicted`), and their oracle names `calculix/static` and its version.
  - Agreement with an in-house run is reported, never folded into a claim.
- `num_modes` is reused as the number of buckling factors. It defaults to 3, because a symmetric section has two equal first modes, as the square column did.
- `load_cases` is refused on a buckling study: the factor scales one load set.
- `StudyInputKey` appends `solver=calculix` only when the solver is not `auto`. Every saved in-house result keeps its hash. A buckling study's key carries its type and `num_modes`.

### The deck: our pipeline in, `.inp` out

The bridge reuses everything up to the solve, so loads and supports mean exactly what they mean in-house:
- `PlanStudy` runs on the main thread;
- `MeshPlannedStudy` produces the Netgen mesh;
- `StudyJobBody` holds the assembled nodal force vector, held nodes, pins, ties and regions.

`WriteDeck(const StudyJobBody&, …)` writes:

| ours | `.inp` |
|---|---|
| nodes (mm), tet10 | `*NODE`; `*ELEMENT, TYPE=C3D10`, one ELSET per `tetDomain` (an assembly's bonded members) |
| material per domain | `*MATERIAL` / `*ELASTIC` (MPa) / `*DENSITY` (t/mm³) / `*SOLID SECTION` |
| force, pressure, torque, remote load, region, gravity | the assembled nodal force vector as `*CLOAD`. These are the forces the in-house solve applies, so every load kind and `region` maps for free and identically. Gravity is in it too. |
| fixed (and a rigid participant's clamp) | `*BOUNDARY n,1,3` on a node set |
| pin: a node held radially and axially in its own frame | `*EQUATION`, one 3-term equation per held direction (d·u = 0). The dependent DOF is the largest component, kept distinct per node, the way `Tie` picks it |
| pin `fix_rotation` (`Tie`) | `*EQUATION` with the tie's own coefficients |
| buckling | `*STEP` / `*BUCKLE` with `num_modes` and accuracy 1e-4 (default 0.01) |
| outputs | `*NODE FILE` U, plus RF for nonlinear runs; `*EL FILE` S, and PE for plasticity |

A pin is compression-only, so its contact set is found by the iteration in `SolveWithPins`. For a buckling study the bridge first runs the in-house static `SolveWithPins` on the same load, then holds the contact set it found bilaterally in the deck. Linear theory scales the load without changing its direction, so the set holds at every factor. The result says the set was found that way.

Anything that does not map is refused by name before a file is written: an unknown load kind, a pin on a modal-on-calculix study, and so on.

New RPC `export_study_inp {id, path}` writes the deck the study would run. It is flagged `NotInBatch`, because it writes a file. It works without ccx installed. With it, a person can open the deck in another tool, and the same-mesh comparison becomes a test.

### Running it

- **The job.** It is the existing study job (`StartStudyRun`, src/ui/SimulationActions.cpp). The worker meshes in-house, writes the deck, and runs ccx through `proc::PipedChild` with cwd set to the run directory (`PipedChild` gains a cwd if it has none).
- **Run directory.** `$XDG_CACHE_HOME/lobole/calculix/<pid>-<job>/`, never inside the project. It is removed on success. On failure it is kept and named in the refusal, and directories of dead pids are swept at start.
- **Threads.** `OMP_NUM_THREADS` and `CCX_NPROC_*` are set to the study's thread budget (docs/simulation/README.md#threads), so a build with a multithreaded solver uses it.
- **Cancel.** `cancel_study`, or the starting connection closing, calls `PipedChild::Kill()` on the pid the job holds.
- **Progress.** The job label names the phase: meshing, writing deck, factoring, eigenvalues. A nonlinear step's fraction is its step time, read from `.sta` as each increment lands.
- **Deadline.** `run_study` gains `timeout_s` (optional; CalculiX runs only). Past it, the process is killed and the run refused, naming the limit. With no deadline the job runs until done or cancelled, as an in-house one does.
- **Memory.** A pre-run estimate (DOF × a per-analysis cost, calibrated by the Stage 1 cost test) refuses a run larger than the memory available. The refusal names both numbers, because SPOOLES has no out-of-core mode and an OOM here takes the host with it. `study_status` reports the estimate.
- **Errors.**
  - A non-zero exit or any `*ERROR` line is refused with the first `*ERROR` line verbatim, plus the run directory.
  - A nonlinear run that stops converging is refused with the load fraction it reached. That fraction is information, not a result, so nothing is stored.

### Results: the same shape, checked the same way

- **Linear runs (static on calculix, buckling's base state).** Only U is read from the `.frd`. The displacement is held to the in-house rule, ‖Ku−F‖ ≤ 1e-6‖F‖, against our own assembled K: same element, same Gauss rule, so this is the same check every in-house answer passes. Stresses, reactions, `peak_at_support_edge`, `max_principal_mpa`, printed-layer stress and probes are then computed by the existing code from that displacement. The result is a `FemResults` like any other.
- **Buckling.** Each body gets:
  - `buckling_factors`: every factor ccx returns, signed. A negative one is the same shape under the reversed load.
  - `buckling_factor`: the smallest positive factor, the governing one. If none of the requested modes is positive, the result says so instead of picking one.
  - mode shapes, stored as modal ones are (`ModeShape` with `factor` beside `frequencyHz`; `KeepWhatIsRead` trims them to boundary nodes).
- **Buckling's honesty rules**, the counterpart of `peak_at_support_edge`:
  - `yield_factor` = yield / peak von Mises of the base (factor 1) state, which is computed anyway. When it is below `buckling_factor`, `yields_first` says the part yields at that multiple before the elastic buckling load is reached. The governing factor is the smaller of the two, and the reply names which.
  - `mode_at_load_edge`: when the first mode's peak motion lies within one element of a support's or a load face's edge, the mode is local crippling where the load enters, not a member buckling. The reply says so and names the face.
  - The receipt's claim is basis `predicted`, with oracle `calculix/buckle` and its version. Its note says linear buckling is an upper bound for imperfection-sensitive shapes (thin shells, tubes).
- **Nonlinear runs (Stage 4 on).** U, S (Cauchy stress at the final increment) and, for plasticity, PE are read from the `.frd`. ccx's equilibrium tolerance is its own (`*CONTROLS`, default 0.5 % of the mean force), not our 1e-6. So the deck tightens it, and the result states the criterion it met and checks that the reactions balance the applied load to that tolerance.
- **The rest is shared.**
  - `study_results` carries `solver: "calculix 2.23"` for these runs.
  - `set_result_display {mode}` selects a buckling mode the way it selects a modal one.
  - The field renderer, deformed shape and mode animation are unchanged.

### Requirements and sweeps

- `study_result` gains the quantity `buckling_factor`, used with `at_least` (src/core/Requirements.cpp, `MeasureStudyResult` in src/remote/RemoteRequirements.cpp).
- `factor_of_safety` on a buckling study reads `yield_factor`.
- Sweeps already list study requirements as not swept ("a study per point is minutes"), and a buckling one joins that list with the same reason.

### GUI

`StudyPanel.cpp` gets:
- **Study type and solver.** `Buckling` in the type combo; a solver combo shown only when there is a choice.
- **When ccx is missing.** The Run button stays enabled and refuses with the install sentence, so the panel never hides why.
- **Buckling results.** A "Buckling factors" list in place of "Natural frequencies". Selecting one shows its mode with the deform slider. The governing factor and `yields_first`/`mode_at_load_edge` are shown as text.
- **Settings.** A `calculix_path` row in Preferences, showing the version found.

### Format

These changes write `vNEXT`:
- the study keys `analysis: "buckling"` and `solver`;
- the result keys `buckling_factors`, `yield_factor` and a mode's `factor`.

Plumbing for the bump:
- raise `kVersionPending`;
- goldens `tests/golden/vNEXT-buckling-study.json` and `.expect.json`;
- a row in format.md's pending table.

Stage 4 onward add the nonlinear keys under their own `vNEXT`.

## Stages

Each stage is useful alone and lands with its docs.

**Stage 1: discovery, deck, linear static on CalculiX**, ~1 200 lines plus tests.
- Contents: `Calculix.{h,cpp}`, `solver` on the study, `export_study_inp`, `ping` features, the `calculix_path` setting, and static on `calculix` through the real pipeline with the residual check.
- `calculix-ccx` goes into shell.nix and into the CI apt list, so the suites run it on 2.23 (nix) and 2.21 (Ubuntu).
- On its own this gives an independent second solve of any linear static study, and a deck any CalculiX user can open.

**Stage 2: buckling**, ~1 000 lines.
- The analysis type, the pin contact set, the results and their honesty flags, the requirement quantity, the GUI list and mode display, `vNEXT`, and docs/simulation/README.md's new section.
- The "Not done" list drops buckling.

**Stage 3: shipping the binary**, ~300 lines plus packaging.
- **AppImage.** Bundle Ubuntu 24.04's `ccx` and its libraries via linuxdeploy `--executable`.
- **Flatpak.** A ccx 2.23 module.
- **Windows.** The installer carries `ccx_static.exe` as `ccx.exe`, pinned by sha256; `packaging/windows/smoke.sh` runs a buckling deck under Wine.
- **Licensing.** `THIRD_PARTY.md` and `SOURCE-OFFER.md` rows, and the ccx source tarball attached to each release. `release-smoke.py` asserts `ping` finds the bundled solver.

**Stage 4: nonlinear static, large deformation**, ~1 000 lines.
- `static` gains `nonlinear: {large_deformation: true, increments?}`, which maps to `NLGEOM` and `INC`.
- Loads:
  - pressure becomes a follower `*DLOAD P` on element faces. A boundary tri6 maps to its tet's face S1–S4, which needs a face map;
  - a remote load or torque becomes `*COUPLING`/`*DISTRIBUTING` on a reference node;
  - a force stays a dead load, and the docs say so.
- Supporting work: progress from `.sta`, the non-convergence refusal, and the equilibrium check.

**Stage 5: contact**, ~1 500 lines.
- A compression-only support against a rigid occurrence or the ground: the partner is meshed coarse and fixed, with `*CONTACT PAIR`.
- An assembly participant role `contact`: meshed separately rather than fused, its touching faces found by the same fuse `PlanAssemblyStudy` already runs, joined by `*CONTACT PAIR` with `friction` (unitless μ). This lifts the one-meshed-occurrence rule for CalculiX studies only.
- Results: contact pressure and the open/closed area per pair, beside the existing reactions.

**Stage 6: plasticity**, ~800 lines.
- `nonlinear.plasticity: true`, with `*PLASTIC` from the material library.
- Metals gain hardening data. A material without it is refused rather than assumed perfectly plastic (see the questions).
- Results: `max_plastic_strain`, a requirement quantity of the same name, and the residual deformation from an unload step.

**Stage 7: thermal**: steady heat transfer, then thermal stress. Sized when it is next.

## Verification

**Closed forms, through `run_study`, tagged `[calculix]`:**
- **Euler column.**
  - Fixed–free 10×10×200 steel to within 0.5 % of π²EI/4L² (measured: +0.06 % at a 2.5 mm mesh).
  - A 10×20 section: the first two factors in the ratio (b/h)² = 4.
- **Thin walls.** A static study's mesh puts one element through a 0.8–2 mm
  wall at the auto size, and that is within 1 % for bending
  ([fem-mesh-quality.md](fem-mesh-quality.md)). Buckling is checked the same
  way: a 1 mm plate's factor at one layer against two or more layers (Netgen
  `closeedgefac` 2, measured at 5–14× the elements). If one layer is off by
  more than 2 %, a buckling study meshes with `closeedgefac`, and that is the
  only change to its mesh.
- **Wide cantilever plate.** The factor carries the plate stiffness Et³/12(1−ν²), not the beam's EI, to within 2 %. A classical simply supported plate needs a one-direction (roller) support that the study does not have.
- **Stage 4.** The elastica: a cantilever's end deflection at PL²/EI = 1 against the Bisshopp–Drucker solution.
- **Stage 5.** Hertz contact width for a sphere on a block; a block lifting off a rigid floor under a moment.
- **Stage 6.** A bar past yield (uniform, exact), and a rectangular beam's collapse load M_p = σ_y b h²/4.

**CalculiX against ours, same mesh:**
- A static study run with `solver: auto` and `solver: calculix` on the same Netgen mesh.
- Parts: the column, a bracket with a pin (rotated-frame holds as `*EQUATION`), a torque and a remote load, a `region` load, and a bonded assembly with two materials.
- Expectation: both pass the 1e-6 residual against our K, and their peak displacement and von Mises agree to 1e-5. Same element and rule, so this measures the deck, not the physics.

**Refusals, in `refused_calls_smoke` and a C++ test:**
- ccx missing (`LOBOLE_CCX` pointing nowhere and an empty PATH): refused naming every install route, with nothing started.
- A ccx failure: a fake `ccx` that prints `*ERROR in …` and exits 1 is refused with that line and the kept run directory.
- `in_house` on buckling; `calculix` on modal or topology; load cases on buckling; non-convergence (Stage 4).
- The deadline: `timeout_s` kills the process and refuses.
- The memory estimate over the available memory.
- Cancel: the ccx pid is gone after `cancel_study`.

**The MCP-only path:** `tests/calculix_smoke.py` builds a column, adds a buckling study and a `buckling_factor` requirement, and reads the factor and `check_requirements`, using RPCs only. It is gated on `ping`'s `features.calculix`.

**Cost:**
- A `[slow]` CPU-time check of a ~240 k-DOF buckling run, against the measured ~48 s.
- The memory estimate's coefficients are taken from the same run, so the estimate cannot drift from what the solver costs.

**Docs:**
- docs/simulation/README.md gains a "CalculiX" section (discovery, the deck table, the honesty rules, the run directory) and loses buckling (later, the rest) from "Not done".
- docs/packaging/README.md gains the bundled solver.
- The RPC `describe` rows for `add_study`, `set_study`, `run_study`, `study_results`, `set_result_display`, `export_study_inp` and `ping`.

## Files

- New: `src/core/fem/Calculix.{h,cpp}`; `tests/test_fem_calculix.cpp`; `tests/calculix_smoke.py`; the goldens.
- `src/core/fem/FemStudy.{h,cpp}`: type, solver, solve branch.
- `src/core/fem/FemSolver.h`: `ModeShape::factor`, buckling fields, `StudyInputKey`.
- `src/core/io/SerializeStudies.cpp`, `src/core/io/SerializationInternal.h`, `docs/versioning/format.md`.
- `src/common/ChildProcess.{h,cpp}`: cwd.
- `src/core/io/Settings.{h,cpp}`: `calculix_path`.
- `src/remote/RemoteSimulation.cpp`: rows and handlers; `src/remote/RemoteDispatch.cpp`: `ping`.
- `src/core/Requirements.cpp`, `src/remote/RemoteRequirements.cpp`.
- `src/ui/SimulationActions.cpp`, `src/ui/StudyPanel.cpp`, `src/ui/SettingsWindow.cpp`.
- The python client's `rpc.py` and `rpc-surface.json`, regenerated.
- `shell.nix`, `.github/workflows/ci.yml`.
- `packaging/appimage/*`, `packaging/flatpak/org.lobole.Lobole.yaml`, `packaging/windows/{installer.nix,installer.nsi,smoke.sh}`.
- `THIRD_PARTY.md`, `LICENSES/SOURCE-OFFER.md`.
